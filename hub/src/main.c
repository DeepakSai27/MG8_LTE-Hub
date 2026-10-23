/*
 * Exactcomms hub on the Minew MG8: downloads C10 tags over BLE and uploads their records over
 * LTE (Quectel EG916Q-GL), a port of the ESP32-S3 hub (exactcomms_esp32-s3-hub).
 *
 * The main thread owns the modem: power, registration, clock, uploads and the status ping.
 * BLE scanning and tag downloads run in the Bluetooth threads and only touch the upload queue.
 * Tag scanning starts once the clock is set, because every record carries a date.
 *
 * LED: red = no LTE, blue = tag connected, green = LTE up and last upload OK,
 *      yellow (red + green) = LTE up but the last upload failed.
 * Logs go to SEGGER RTT. tools/flash.ps1 shuts the modem down cleanly before flashing.
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>
#include <nrf.h>

#include "clock.h"
#include "modem.h"
#include "tag_client.h"
#include "upload.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* The MG8 has no DC/DC inductors: either converter browns the chip out in a loop */
#if defined(CONFIG_SOC_DCDC_NRF52X) || defined(CONFIG_SOC_DCDC_NRF52X_HV)
#error "DC/DC must stay off on the MG8 (build for board mg8_nrf52840)"
#endif

#define LED_RED	  NRF_GPIO_PIN_MAP(0, 8)
#define LED_GREEN NRF_GPIO_PIN_MAP(0, 6)
#define LED_BLUE  NRF_GPIO_PIN_MAP(0, 12)

#define NETWORK_CHECK_MS     10000
#define NETWORK_RECHECK_MS   60000
#define NETWORK_LOST_RESTART (5 * 60 * 1000)
#define POWER_RETRY_MS	     30000
#define TIME_RESYNC_MS	     (6 * 3600 * 1000LL)
#define TIME_RETRY_MS	     60000

static void led_set(bool red, bool green, bool blue)
{
	nrf_gpio_pin_write(LED_RED, !red);
	nrf_gpio_pin_write(LED_GREEN, !green);
	nrf_gpio_pin_write(LED_BLUE, !blue);
}

static void leds_init(void)
{
	nrf_gpio_pin_set(LED_RED);
	nrf_gpio_pin_set(LED_GREEN);
	nrf_gpio_pin_set(LED_BLUE);
	nrf_gpio_cfg_output(LED_RED);
	nrf_gpio_cfg_output(LED_GREEN);
	nrf_gpio_cfg_output(LED_BLUE);
}

/*
 * REGOUT0 must be 3.0 V (the erased default 1.8 V is too low for the LEDs and flash).
 * APPROTECT is never touched: on this AAD0 chip any value other than 0xFF locks it.
 */
static void uicr_setup(void)
{
	uint32_t want = (NRF_UICR->REGOUT0 & ~UICR_REGOUT0_VOUT_Msk) |
			(UICR_REGOUT0_VOUT_3V0 << UICR_REGOUT0_VOUT_Pos);

	if (NRF_UICR->REGOUT0 == want) {
		return;
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
	while (!NRF_NVMC->READY) {
	}
	NRF_UICR->REGOUT0 = want;
	while (!NRF_NVMC->READY) {
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
	while (!NRF_NVMC->READY) {
	}
	NVIC_SystemReset();
}

static void set_hub_id(void)
{
	bt_addr_le_t addr;
	size_t count = 1;
	char mac[18];

	bt_id_get(&addr, &count);
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", addr.a.val[5], addr.a.val[4],
		 addr.a.val[3], addr.a.val[2], addr.a.val[1], addr.a.val[0]);
	upload_set_hub_id(mac);
	LOG_INF("Hub ID (BLE address) %s", mac);
}

int main(void)
{
	uicr_setup();
	leds_init();
	led_set(true, false, false);

	uint32_t resetreas = NRF_POWER->RESETREAS;

	NRF_POWER->RESETREAS = resetreas;
	NRF_POWER->GPREGRET2 = 0;
	upload_set_reset_reason(resetreas);
	upload_init();

	LOG_INF("MG8 hub %s, RESETREAS 0x%08x, DCDCEN %u/%u", CONFIG_HUB_VERSION, resetreas,
		NRF_POWER->DCDCEN, NRF_POWER->DCDCEN0);
	LOG_INF("Upload servers %s / %s / %s port %d%s", CONFIG_HUB_SERVER_MAIN,
		CONFIG_HUB_SERVER_AZ2, CONFIG_HUB_SERVER_AZ3, CONFIG_HUB_SERVER_PORT,
		IS_ENABLED(CONFIG_HUB_UPLOAD_APPEND_NEWLINE) ? " (newline appended, test mode)" : "");

	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
	} else {
		set_hub_id();
	}

	if (modem_init() != 0) {
		led_set(true, false, false);
		return 0;
	}

	bool network_ok = false;
	int64_t last_power_try = -POWER_RETRY_MS;
	int64_t last_network_check = -NETWORK_CHECK_MS;
	int64_t network_lost_since = k_uptime_get();
	int64_t last_time_sync = 0;
	int64_t last_time_try = -TIME_RETRY_MS;
	int64_t last_ping = 0;

	for (;;) {
		int64_t now = k_uptime_get();

		if (modem_check_shutdown_request()) {
			/* Flashing or a debugger reset follows; stay quiet until then */
			led_set(false, false, false);
			for (;;) {
				k_sleep(K_FOREVER);
			}
		}

		if (!modem_is_on()) {
			network_ok = false;
			if (now - last_power_try >= POWER_RETRY_MS) {
				last_power_try = now;
				if (modem_power_on() != 0) {
					modem_power_off();
				}
				network_lost_since = k_uptime_get();
			}
			k_msleep(100);
			continue;
		}

		if (modem_is_stuck()) {
			LOG_WRN("Modem stopped answering, power-cycling it");
			modem_power_off();
			continue;
		}

		/* Registration and PDP context: often while down, once a minute while up */
		if (now - last_network_check >= (network_ok ? NETWORK_RECHECK_MS : NETWORK_CHECK_MS)) {
			last_network_check = now;
			bool ok = modem_network_ready();

			if (ok && !network_ok) {
				LOG_INF("LTE up, signal %d dBm", modem_signal_dbm());
			} else if (!ok && network_ok) {
				LOG_WRN("LTE lost");
				network_lost_since = now;
			} else if (!ok && now - network_lost_since > NETWORK_LOST_RESTART) {
				LOG_WRN("No LTE for %d min, power-cycling the modem",
					NETWORK_LOST_RESTART / 60000);
				modem_power_off();
				continue;
			}
			network_ok = ok;
		}

		if (network_ok) {
			if ((!clock_is_set() || now - last_time_sync > TIME_RESYNC_MS) &&
			    now - last_time_try >= TIME_RETRY_MS) {
				last_time_try = now;
				if (modem_sync_time() == 0) {
					last_time_sync = now;
				}
			}
			if (clock_is_set()) {
				tag_client_start();

				/* First ping right away so a new hub shows up, then periodically */
				if (last_ping == 0 ||
				    now - last_ping >= CONFIG_HUB_STATUS_PING_MIN * 60 * 1000LL) {
					last_ping = now;
					upload_queue_status_ping(modem_signal_dbm());
				}
			}
			upload_process();
		}

		if (tag_client_busy()) {
			led_set(false, false, true);
		} else if (!network_ok) {
			led_set(true, false, false);
		} else {
			led_set(!upload_last_sync_ok(), true, false);
		}
		k_msleep(100);
	}
	return 0;
}
