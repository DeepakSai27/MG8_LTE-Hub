/*
 * Minew MG8 bring-up: finds which nRF52840 pins power up and talk to the Quectel modem.
 *
 * Known MG8 pins (found by J-Link probing):
 *   LEDs (active low): red P0.08, green P0.06, blue P0.12
 *   Button (active low, needs pull-up): P0.04
 *   Debug pads: TX P0.13, RX P0.15
 *   P0.09 / P0.10: held low on the board, PWRKEY / RESET candidates
 *
 * Search:
 *   1. Pull every unknown pin up, so any active-high power or level-shifter enable is on. Pins are
 *      never driven: pushing 37 pins high overloaded the 25 mA REGOUT0 supply (VDD sagged to 2.2 V).
 *   2. Pulse each PWRKEY candidate and watch for pins that the level shifter starts driving.
 *   3. Try every pair of those pins as a 115200 UART until the modem answers "AT" with "OK".
 *   4. Release the driven pins one at a time to see which ones the modem needs.
 * All output goes to SEGGER RTT (channel 0). Press the button to run the search again.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <nrf.h>

#define PIN(port, n) NRF_GPIO_PIN_MAP(port, n)

#define LED_RED	  PIN(0, 8)
#define LED_GREEN PIN(0, 6)
#define LED_BLUE  PIN(0, 12)
#define BUTTON	  PIN(0, 4)

#define PWRKEY_PULSE_MS	   1200
#define MODEM_BOOT_WAIT_S  15
#define AT_REPLY_WAIT_MS   300
#define MODEM_REPOWER_WAIT 20

static const uint32_t pwrkey_candidates[] = {PIN(0, 9), PIN(0, 10)};

/* Every pin that is not a known function, the 32 kHz crystal (P0.00/P0.01) or the reset pin (P0.18) */
static const uint32_t candidates[] = {
	PIN(0, 2),  PIN(0, 3),	PIN(0, 5),  PIN(0, 7),	PIN(0, 11), PIN(0, 14), PIN(0, 16),
	PIN(0, 17), PIN(0, 19), PIN(0, 20), PIN(0, 21), PIN(0, 22), PIN(0, 23), PIN(0, 24),
	PIN(0, 25), PIN(0, 26), PIN(0, 27), PIN(0, 28), PIN(0, 29), PIN(0, 30), PIN(0, 31),
	PIN(1, 0),  PIN(1, 1),	PIN(1, 2),  PIN(1, 3),	PIN(1, 4),  PIN(1, 5),	PIN(1, 6),
	PIN(1, 7),  PIN(1, 8),	PIN(1, 9),  PIN(1, 10), PIN(1, 11), PIN(1, 12), PIN(1, 13),
	PIN(1, 14), PIN(1, 15),
};

#define NUM_CANDIDATES ARRAY_SIZE(candidates)

static bool pin_high[NUM_CANDIDATES];

static uint8_t uart_tx_buf[32];
static uint8_t uart_rx_buf[512];
static char reply[512];

static uint32_t modem_tx = UINT32_MAX; /* nRF pin that receives the modem output */
static uint32_t modem_rx = UINT32_MAX; /* nRF pin that drives the modem input */
static uint32_t modem_pwrkey = UINT32_MAX;

static void print_pin(uint32_t pin)
{
	printk("P%u.%02u", pin >> 5, pin & 0x1F);
}

static bool uicr_write(volatile uint32_t *reg, uint32_t value)
{
	if (*reg == value) {
		return false;
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
	while (!NRF_NVMC->READY) {
	}
	*reg = value;
	while (!NRF_NVMC->READY) {
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
	while (!NRF_NVMC->READY) {
	}
	return true;
}

/**
 * The MG8 needs REGOUT0 at 3.0 V: the erased default of 1.8 V is too low for the LEDs and the
 * external flash. A chip erase clears it, so it is restored on every boot.
 *
 * UICR.APPROTECT is deliberately left alone. The MG8 nRF52840 (variant AAD0) uses the legacy
 * scheme where only the erased value 0xFF keeps the debug port open; any other value, including
 * the newer revisions' HwDisabled (0x5A), locks it.
 */
static void uicr_setup(void)
{
	bool changed = false;

	if ((NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) !=
	    (UICR_REGOUT0_VOUT_3V0 << UICR_REGOUT0_VOUT_Pos)) {
		changed |= uicr_write(&NRF_UICR->REGOUT0,
				      (NRF_UICR->REGOUT0 & ~UICR_REGOUT0_VOUT_Msk) |
					      (UICR_REGOUT0_VOUT_3V0 << UICR_REGOUT0_VOUT_Pos));
	}
	if (changed) {
		NVIC_SystemReset();
	}
}

static void led_set(bool red, bool green, bool blue)
{
	nrf_gpio_pin_write(LED_RED, !red);
	nrf_gpio_pin_write(LED_GREEN, !green);
	nrf_gpio_pin_write(LED_BLUE, !blue);
}

static void board_init(void)
{
	nrf_gpio_pin_set(LED_RED);
	nrf_gpio_pin_set(LED_GREEN);
	nrf_gpio_pin_set(LED_BLUE);
	nrf_gpio_cfg_output(LED_RED);
	nrf_gpio_cfg_output(LED_GREEN);
	nrf_gpio_cfg_output(LED_BLUE);
	nrf_gpio_cfg_input(BUTTON, NRF_GPIO_PIN_PULLUP);

	for (size_t i = 0; i < ARRAY_SIZE(pwrkey_candidates); i++) {
		nrf_gpio_pin_clear(pwrkey_candidates[i]);
		nrf_gpio_cfg_output(pwrkey_candidates[i]);
	}
}

/* A pull-up is enough to turn on a high-impedance enable input and limits the current to ~0.25 mA */
static void pull_high(uint32_t pin)
{
	nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLUP);
}

/**
 * Switches the pin to a pull-down and reads it. A pin that still reads high is driven by
 * something else (the level shifter) and is left as a plain input. A pin that reads low goes
 * back to the pull-up, in case it is an enable.
 */
static bool sense(uint32_t pin)
{
	nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLDOWN);
	k_busy_wait(300);
	bool high = nrf_gpio_pin_read(pin);

	if (high) {
		nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_NOPULL);
	} else {
		pull_high(pin);
	}
	return high;
}

static int sense_all(const char *label)
{
	int count = 0;

	for (size_t i = 0; i < NUM_CANDIDATES; i++) {
		pin_high[i] = sense(candidates[i]);
		count += pin_high[i];
	}
	if (label != NULL) {
		printk("[%s] %d pin(s) driven high:", label, count);
		for (size_t i = 0; i < NUM_CANDIDATES; i++) {
			if (pin_high[i]) {
				printk(" ");
				print_pin(candidates[i]);
			}
		}
		printk("\n");
	}
	return count;
}

/**
 * Sends a command on UARTE1 using the given pins and collects the reply for wait_ms.
 * The reply is copied into the global reply buffer with non-printable bytes replaced.
 */
static size_t uart_exchange(uint32_t tx, uint32_t rx, const char *cmd, int wait_ms)
{
	NRF_UARTE_Type *uarte = NRF_UARTE1;
	size_t len = strlen(cmd);

	/* The UARTE drives TX itself once enabled; setting the output level first avoids a start glitch */
	nrf_gpio_pin_set(tx);
	nrf_gpio_cfg_output(tx);
	nrf_gpio_cfg_input(rx, NRF_GPIO_PIN_NOPULL);

	uarte->ENABLE = UARTE_ENABLE_ENABLE_Disabled;
	uarte->PSEL.TXD = tx;
	uarte->PSEL.RXD = rx;
	uarte->PSEL.RTS = UINT32_MAX;
	uarte->PSEL.CTS = UINT32_MAX;
	uarte->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud115200;
	uarte->CONFIG = 0;
	uarte->ENABLE = UARTE_ENABLE_ENABLE_Enabled;

	memset(uart_rx_buf, 0, sizeof(uart_rx_buf));
	uarte->EVENTS_ENDRX = 0;
	uarte->EVENTS_RXTO = 0;
	uarte->EVENTS_ENDTX = 0;
	uarte->RXD.PTR = (uint32_t)uart_rx_buf;
	uarte->RXD.MAXCNT = sizeof(uart_rx_buf) - 1;
	uarte->TASKS_STARTRX = 1;

	memcpy(uart_tx_buf, cmd, MIN(len, sizeof(uart_tx_buf)));
	uarte->TXD.PTR = (uint32_t)uart_tx_buf;
	uarte->TXD.MAXCNT = MIN(len, sizeof(uart_tx_buf));
	uarte->TASKS_STARTTX = 1;
	for (int i = 0; i < 200 && !uarte->EVENTS_ENDTX; i++) {
		k_busy_wait(100);
	}
	uarte->TASKS_STOPTX = 1;

	k_msleep(wait_ms);

	uarte->TASKS_STOPRX = 1;
	for (int i = 0; i < 200 && !uarte->EVENTS_RXTO; i++) {
		k_busy_wait(100);
	}
	size_t amount = uarte->RXD.AMOUNT;

	uarte->ENABLE = UARTE_ENABLE_ENABLE_Disabled;
	uarte->PSEL.TXD = UINT32_MAX;
	uarte->PSEL.RXD = UINT32_MAX;

	for (size_t i = 0; i < amount && i < sizeof(reply) - 1; i++) {
		char c = uart_rx_buf[i];

		reply[i] = (c == '\r' || c == '\n' || (c >= 0x20 && c < 0x7F)) ? c : '.';
	}
	reply[MIN(amount, sizeof(reply) - 1)] = '\0';
	return amount;
}

static bool at_ok(uint32_t tx, uint32_t rx)
{
	/* The first AT also lets an auto-bauding modem lock onto the baud rate */
	uart_exchange(tx, rx, "AT\r", 100);
	uart_exchange(tx, rx, "AT\r", AT_REPLY_WAIT_MS);
	return strstr(reply, "OK") != NULL;
}

static bool try_uart_pairs(void)
{
	uint32_t high[NUM_CANDIDATES];
	size_t count = 0;

	for (size_t i = 0; i < NUM_CANDIDATES; i++) {
		if (pin_high[i]) {
			high[count++] = candidates[i];
		}
	}
	if (count < 2) {
		printk("Fewer than 2 driven pins, no UART pairs to try\n");
		return false;
	}

	printk("Trying %u UART pin pairs at 115200...\n", count * (count - 1));
	for (size_t t = 0; t < count; t++) {
		for (size_t r = 0; r < count; r++) {
			if (t == r) {
				continue;
			}
			bool ok = at_ok(high[t], high[r]);

			/* The TX candidate goes back to a plain input, it is driven by the level shifter */
			nrf_gpio_cfg_input(high[t], NRF_GPIO_PIN_NOPULL);
			if (ok) {
				modem_rx = high[t];
				modem_tx = high[r];
				printk("MODEM FOUND: nRF TX ");
				print_pin(modem_rx);
				printk(" -> modem RX, modem TX -> nRF RX ");
				print_pin(modem_tx);
				printk("\n");
				return true;
			}
		}
	}
	return false;
}

static void pulse(uint32_t pin, int ms)
{
	nrf_gpio_pin_set(pin);
	k_msleep(ms);
	nrf_gpio_pin_clear(pin);
}

static void modem_command(const char *cmd, int wait_ms)
{
	uart_exchange(modem_rx, modem_tx, cmd, wait_ms);
	nrf_gpio_cfg_input(modem_rx, NRF_GPIO_PIN_NOPULL);
	printk(">> %s\n%s\n", cmd, reply);
}

static void modem_query(void)
{
	printk("\n--- Modem information ---\n");
	modem_command("ATE0\r", 300);
	modem_command("ATI\r", 500);
	modem_command("AT+CGMR\r", 500);
	modem_command("AT+QCCID\r", 500);
	modem_command("AT+CPIN?\r", 500);
	modem_command("AT+CSQ\r", 500);
	modem_command("AT+CEREG?\r", 500);
	modem_command("AT+COPS?\r", 1000);
}

static bool modem_wait_ready(int seconds)
{
	for (int s = 0; s < seconds; s++) {
		bool ok = at_ok(modem_rx, modem_tx);

		nrf_gpio_cfg_input(modem_rx, NRF_GPIO_PIN_NOPULL);
		if (ok) {
			return true;
		}
		k_msleep(1000 - 2 * AT_REPLY_WAIT_MS);
	}
	return false;
}

/**
 * Releases each pin the firmware drives high, one at a time, and checks whether the modem still
 * answers. A pin whose release breaks AT but where AT returns as soon as it is driven again is a
 * level-shifter style enable; one that needs a new PWRKEY pulse powers the modem.
 */
static void find_required_pins(void)
{
	printk("\n--- Checking which driven pins the modem needs ---\n");
	for (size_t i = 0; i < NUM_CANDIDATES; i++) {
		uint32_t pin = candidates[i];

		if (pin_high[i] || pin == modem_tx || pin == modem_rx) {
			continue;
		}
		nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLDOWN);
		k_msleep(50);
		bool still_ok = at_ok(modem_rx, modem_tx);

		nrf_gpio_cfg_input(modem_rx, NRF_GPIO_PIN_NOPULL);
		pull_high(pin);
		if (still_ok) {
			continue;
		}

		print_pin(pin);
		k_msleep(50);
		if (modem_wait_ready(2)) {
			printk(" is NEEDED: modem stops answering while it is low (enable for the UART path)\n");
			continue;
		}
		printk(" is NEEDED: modem lost power while it was low, powering it up again\n");
		pulse(modem_pwrkey, PWRKEY_PULSE_MS);
		if (!modem_wait_ready(MODEM_REPOWER_WAIT)) {
			printk("Modem did not come back, stopping the check\n");
			return;
		}
	}
	printk("Check done\n");
}

static bool run_search(void)
{
	modem_tx = modem_rx = modem_pwrkey = UINT32_MAX;
	led_set(false, false, true);

	printk("\n=== Modem search ===\n");
	printk("Pulling %u unknown pins up, PWRKEY candidates low\n", NUM_CANDIDATES);
	for (size_t i = 0; i < NUM_CANDIDATES; i++) {
		pull_high(candidates[i]);
	}
	k_msleep(500);

	sense_all("baseline");
	if (try_uart_pairs()) {
		printk("Modem was already on\n");
	} else {
		for (size_t p = 0; p < ARRAY_SIZE(pwrkey_candidates); p++) {
			uint32_t pwrkey = pwrkey_candidates[p];
			int last = -1;

			printk("\nPulsing ");
			print_pin(pwrkey);
			printk(" high for %d ms\n", PWRKEY_PULSE_MS);
			pulse(pwrkey, PWRKEY_PULSE_MS);

			for (int s = 1; s <= MODEM_BOOT_WAIT_S; s++) {
				k_msleep(1000);
				int count = sense_all(NULL);

				if (count != last) {
					printk("  t+%ds: %d pin(s) driven high\n", s, count);
					last = count;
				}
			}
			sense_all("after pulse");
			if (try_uart_pairs()) {
				modem_pwrkey = pwrkey;
				printk("PWRKEY is ");
				print_pin(pwrkey);
				printk("\n");
				break;
			}
		}
	}

	if (modem_tx == UINT32_MAX) {
		led_set(true, false, false);
		printk("\nNo modem answered. Press the button to search again.\n");
		return false;
	}

	led_set(false, true, false);
	modem_query();
	if (modem_pwrkey != UINT32_MAX) {
		find_required_pins();
	}
	printk("\nSearch finished. Press the button to run it again.\n");
	return true;
}

int main(void)
{
	uicr_setup();
	board_init();

	uint32_t reset_reason = NRF_POWER->RESETREAS;

	NRF_POWER->RESETREAS = reset_reason;

	/* Gives the RTT host time to attach before the first output */
	led_set(false, false, true);
	k_msleep(3000);

	printk("\n*** MG8 bring-up (%s %s) ***\n", __DATE__, __TIME__);
	printk("RESETREAS 0x%08x, UICR REGOUT0 0x%08x, APPROTECT 0x%08x, NFCPINS 0x%08x\n",
	       reset_reason, NRF_UICR->REGOUT0, NRF_UICR->APPROTECT, NRF_UICR->NFCPINS);

	run_search();

	for (;;) {
		if (!nrf_gpio_pin_read(BUTTON)) {
			k_msleep(50);
			while (!nrf_gpio_pin_read(BUTTON)) {
				k_msleep(10);
			}
			run_search();
		}
		k_msleep(20);
	}
	return 0;
}
