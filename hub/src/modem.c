#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <hal/nrf_gpio.h>
#include <nrf.h>

#include "clock.h"
#include "modem.h"

LOG_MODULE_REGISTER(modem, LOG_LEVEL_INF);

/* Modem control pins, see README.md. Each drives an N-MOSFET or the buck enable: high = active. */
#define MODEM_PWR_EN	NRF_GPIO_PIN_MAP(1, 9)
#define MODEM_PWRKEY	NRF_GPIO_PIN_MAP(0, 15)
#define MODEM_RESET	NRF_GPIO_PIN_MAP(0, 13)
#define MODEM_W_DISABLE NRF_GPIO_PIN_MAP(0, 31)
#define MODEM_DTR	NRF_GPIO_PIN_MAP(0, 16)
#define MODEM_RTS	NRF_GPIO_PIN_MAP(0, 24)

#define PWRKEY_PULSE_MS	 1000
#define BOOT_TIMEOUT_S	 20
#define QISEND_MAX	 1460
#define SHUTDOWN_REQUEST 0xA5
#define SHUTDOWN_DONE	 0x5A

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

RING_BUF_DECLARE(rx_ring, 2048);
static K_SEM_DEFINE(rx_sem, 0, 1);

/* Asynchronous results that can arrive while another command runs */
#define URC_SLOTS 4
static char urc_store[URC_SLOTS][128];

static char line[600];
static char resp[1024];
static bool socket_closed;

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[32];

	if (!uart_irq_update(dev)) {
		return;
	}
	while (uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		ring_buf_put(&rx_ring, buf, n);
		k_sem_give(&rx_sem);
	}
}

static int get_char(int64_t deadline)
{
	for (;;) {
		uint8_t c;
		unsigned int key = irq_lock();
		uint32_t n = ring_buf_get(&rx_ring, &c, 1);

		irq_unlock(key);
		if (n == 1) {
			return c;
		}
		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		k_sem_take(&rx_sem, K_MSEC(left));
	}
}

/* One non-empty line without CR/LF, or -ETIMEDOUT */
static int read_line(char *buf, size_t len, int64_t deadline)
{
	size_t n = 0;

	for (;;) {
		int c = get_char(deadline);

		if (c < 0) {
			return c;
		}
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			if (n == 0) {
				continue;
			}
			buf[n] = '\0';
			return n;
		}
		if (n < len - 1) {
			buf[n++] = (char)c;
		}
	}
}

static void write_str(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart, s[i]);
	}
}

static bool is_async(const char *l)
{
	return strncmp(l, "+QIOPEN:", 8) == 0 || strncmp(l, "+QIURC:", 7) == 0 ||
	       strncmp(l, "+QNTP:", 6) == 0;
}

static void handle_async(const char *l)
{
	if (strncmp(l, "+QIURC: \"closed\"", 16) == 0) {
		socket_closed = true;
	}
	if (strncmp(l, "+QIURC: \"recv\"", 14) == 0) {
		return;
	}
	for (int i = 0; i < URC_SLOTS; i++) {
		if (urc_store[i][0] == '\0') {
			strncpy(urc_store[i], l, sizeof(urc_store[i]) - 1);
			return;
		}
	}
	LOG_WRN("URC store full, dropping: %s", l);
}

/* Takes a stored asynchronous line starting with prefix, if any */
static bool take_urc(const char *prefix, char *out, size_t len)
{
	for (int i = 0; i < URC_SLOTS; i++) {
		if (urc_store[i][0] != '\0' && strncmp(urc_store[i], prefix, strlen(prefix)) == 0) {
			strncpy(out, urc_store[i], len - 1);
			out[len - 1] = '\0';
			urc_store[i][0] = '\0';
			return true;
		}
	}
	return false;
}

static void clear_urcs(const char *prefix)
{
	char tmp[128];

	while (take_urc(prefix, tmp, sizeof(tmp))) {
	}
}

/* Reads whatever arrived between commands: URCs and stray lines */
static void drain(void)
{
	while (read_line(line, sizeof(line), k_uptime_get()) > 0) {
		if (is_async(line)) {
			handle_async(line);
		} else {
			LOG_DBG("unsolicited: %s", line);
		}
	}
}

/*
 * Sends cmd and collects the response lines (without the final OK) into resp.
 * Returns 0 on OK, -EIO on ERROR, -ETIMEDOUT.
 */
static int at_cmd(const char *cmd, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	size_t used = 0;

	drain();
	resp[0] = '\0';
	write_str(cmd, strlen(cmd));
	write_str("\r", 1);

	while (read_line(line, sizeof(line), deadline) > 0) {
		if (strcmp(line, "OK") == 0) {
			return 0;
		}
		if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0) {
			LOG_WRN("%s -> %s", cmd, line);
			return -EIO;
		}
		if (is_async(line)) {
			handle_async(line);
			continue;
		}
		if (strcmp(line, cmd) == 0) {
			continue; /* echo, before ATE0 */
		}
		used += snprintf(resp + used, sizeof(resp) - used, "%s\n", line);
		if (used >= sizeof(resp)) {
			used = sizeof(resp) - 1;
		}
	}
	LOG_WRN("%s -> timeout", cmd);
	return -ETIMEDOUT;
}

/* Waits for an asynchronous line with the given prefix (stored or new) */
static int wait_urc(const char *prefix, char *out, size_t len, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	for (;;) {
		if (take_urc(prefix, out, len)) {
			return 0;
		}
		if (read_line(line, sizeof(line), deadline) < 0) {
			return -ETIMEDOUT;
		}
		if (is_async(line)) {
			handle_async(line);
		}
	}
}

static void output_low(uint32_t pin)
{
	nrf_gpio_pin_clear(pin);
	nrf_gpio_cfg_output(pin);
}

int modem_init(void)
{
	/* Modem off, PWRKEY released, not in reset, not in airplane mode, DTR/RTS asserted */
	output_low(MODEM_PWR_EN);
	output_low(MODEM_PWRKEY);
	output_low(MODEM_RESET);
	output_low(MODEM_W_DISABLE);
	output_low(MODEM_DTR);
	output_low(MODEM_RTS);

	if (!device_is_ready(uart)) {
		LOG_ERR("uart1 not ready");
		return -ENODEV;
	}
	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);
	return 0;
}

bool modem_is_on(void)
{
	return nrf_gpio_pin_out_read(MODEM_PWR_EN);
}

int modem_power_on(void)
{
	LOG_INF("Powering the modem up");
	nrf_gpio_pin_set(MODEM_PWR_EN);
	k_msleep(100);
	nrf_gpio_pin_set(MODEM_PWRKEY);
	k_msleep(PWRKEY_PULSE_MS);
	nrf_gpio_pin_clear(MODEM_PWRKEY);

	for (int s = 0; s < BOOT_TIMEOUT_S; s++) {
		if (at_cmd("AT", 500) == 0) {
			LOG_INF("Modem answered after %d s", s + 1);
			at_cmd("ATE0", 500);
			at_cmd("AT+CMEE=2", 500);
			if (at_cmd("ATI", 1000) == 0) {
				LOG_INF("%s", resp);
			}
			if (strlen(CONFIG_HUB_APN) > 0) {
				char cmd[96];

				snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",1",
					 CONFIG_HUB_APN);
				at_cmd(cmd, 1000);
			}
			return 0;
		}
		k_msleep(500);
	}
	LOG_ERR("Modem did not answer within %d s", BOOT_TIMEOUT_S);
	return -ETIMEDOUT;
}

void modem_power_off(void)
{
	/* Cutting the supply under a running modem can corrupt its file system */
	if (modem_is_on() && at_cmd("AT", 500) == 0) {
		LOG_INF("Shutting the modem down (AT+QPOWD)");
		at_cmd("AT+QPOWD=1", 2000);
		k_msleep(3000);
	}
	nrf_gpio_pin_clear(MODEM_PWRKEY);
	nrf_gpio_pin_clear(MODEM_PWR_EN);
	socket_closed = true;
}

static int cereg_status(void)
{
	if (at_cmd("AT+CEREG?", 1000) != 0) {
		return -1;
	}
	char *p = strstr(resp, "+CEREG:");
	char *comma = (p != NULL) ? strchr(p, ',') : NULL;

	return (comma != NULL) ? atoi(comma + 1) : -1;
}

bool modem_network_ready(void)
{
	int stat = cereg_status();

	if (stat != 1 && stat != 5) {
		return false;
	}
	if (at_cmd("AT+QIACT?", 2000) == 0 && strstr(resp, "+QIACT: 1,1") != NULL) {
		return true;
	}
	LOG_INF("Activating the PDP context");
	if (at_cmd("AT+QIACT=1", 150000) != 0) {
		return false;
	}
	return at_cmd("AT+QIACT?", 2000) == 0 && strstr(resp, "+QIACT: 1,1") != NULL;
}

int modem_signal_dbm(void)
{
	if (at_cmd("AT+CSQ", 1000) != 0) {
		return 0;
	}
	char *p = strstr(resp, "+CSQ:");
	int rssi = (p != NULL) ? atoi(p + 5) : 99;

	return (rssi >= 0 && rssi <= 31) ? -113 + 2 * rssi : 0;
}

/* "yyyy/MM/dd,hh:mm:ss+zz" (zz = quarter hours, year may be two digits) to UTC epoch */
static int64_t parse_modem_time(const char *s)
{
	int y, mo, d, h, mi, sec, tz = 0;
	char sign = '+';
	const char *q = strchr(s, '"');

	if (q == NULL || sscanf(q + 1, "%d/%d/%d,%d:%d:%d%c%d", &y, &mo, &d, &h, &mi, &sec, &sign,
				&tz) < 6) {
		return -1;
	}
	/* An unset modem clock reads "70/01/01": two-digit years from 70 are 19xx */
	if (y < 70) {
		y += 2000;
	} else if (y < 100) {
		y += 1900;
	}
	if (y < 2024) {
		LOG_WRN("Modem clock not set (%s)", s);
		return -1;
	}
	int64_t epoch = clock_epoch_from_civil(y, mo, d, h, mi, sec);
	int64_t offset = (int64_t)tz * 15 * 60;

	return (sign == '-') ? epoch + offset : epoch - offset;
}

int modem_sync_time(void)
{
	char cmd[96];
	char urc[128];
	int64_t epoch = -1;

	clear_urcs("+QNTP:");
	snprintf(cmd, sizeof(cmd), "AT+QNTP=1,\"%s\",123", CONFIG_HUB_NTP_SERVER);
	if (at_cmd(cmd, 5000) == 0 && wait_urc("+QNTP:", urc, sizeof(urc), 70000) == 0) {
		LOG_INF("NTP: %s", urc);
		if (atoi(urc + 6) == 0) {
			epoch = parse_modem_time(urc);
		} else {
			LOG_WRN("NTP failed: %s", urc);
		}
	}
	if (epoch < 0 && at_cmd("AT+CCLK?", 1000) == 0) {
		LOG_INF("NTP unavailable, using the network time: %s", resp);
		epoch = parse_modem_time(resp);
	}
	if (epoch < 0) {
		return -EAGAIN;
	}
	clock_set_utc(epoch);

	char now[24];

	clock_format_datetime(epoch, now, sizeof(now));
	LOG_INF("Clock set: %s UTC", now);
	return 0;
}

int modem_tcp_open(const char *host, uint16_t port)
{
	char cmd[128];
	char urc[64];

	at_cmd("AT+QICLOSE=0", 10000);
	clear_urcs("+QIOPEN:");
	clear_urcs("+QIURC:");
	socket_closed = false;

	snprintf(cmd, sizeof(cmd), "AT+QIOPEN=1,0,\"TCP\",\"%s\",%u,0,0", host, port);
	if (at_cmd(cmd, 5000) != 0) {
		return -EIO;
	}
	if (wait_urc("+QIOPEN: 0,", urc, sizeof(urc), 60000) != 0) {
		LOG_WRN("No +QIOPEN result for %s:%u", host, port);
		return -ETIMEDOUT;
	}
	int err = atoi(urc + 11);

	if (err != 0) {
		LOG_WRN("Connecting to %s:%u failed, error %d", host, port, err);
		return -ECONNREFUSED;
	}
	return 0;
}

static int send_chunk(const char *data, size_t len)
{
	char cmd[32];
	int64_t deadline = k_uptime_get() + 5000;
	size_t n = 0;

	drain();
	snprintf(cmd, sizeof(cmd), "AT+QISEND=0,%u\r", len);
	write_str(cmd, strlen(cmd));

	/* The modem answers with a "> " prompt (no newline) when it is ready for the data */
	for (;;) {
		int c = get_char(deadline);

		if (c < 0) {
			return -ETIMEDOUT;
		}
		if (c == '>') {
			break;
		}
		if (c == '\n') {
			line[n] = '\0';
			if (strstr(line, "ERROR") != NULL) {
				LOG_WRN("QISEND refused: %s", line);
				return -EIO;
			}
			n = 0;
		} else if (c != '\r' && n < sizeof(line) - 1) {
			line[n++] = (char)c;
		}
	}
	write_str(data, len);

	deadline = k_uptime_get() + 10000;
	while (read_line(line, sizeof(line), deadline) > 0) {
		if (strcmp(line, "SEND OK") == 0) {
			return 0;
		}
		if (strcmp(line, "SEND FAIL") == 0 || strstr(line, "ERROR") != NULL) {
			LOG_WRN("Send failed: %s", line);
			return -EIO;
		}
		if (is_async(line)) {
			handle_async(line);
		}
	}
	return -ETIMEDOUT;
}

int modem_tcp_send(const void *data, size_t len)
{
	const char *p = data;

	while (len > 0) {
		size_t chunk = MIN(len, QISEND_MAX);
		int err = send_chunk(p, chunk);

		if (err) {
			return err;
		}
		p += chunk;
		len -= chunk;
	}
	return 0;
}

int modem_tcp_wait_reply(int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	do {
		if (at_cmd("AT+QIRD=0,1500", 2000) == 0) {
			char *p = strstr(resp, "+QIRD:");
			int n = (p != NULL) ? atoi(p + 6) : 0;

			if (n > 0) {
				char *data = strchr(p, '\n');

				LOG_INF("Server replied %d bytes: %.40s", n, data != NULL ? data + 1 : "");
				return n;
			}
		}
		if (socket_closed) {
			return -ENOTCONN;
		}
		k_msleep(300);
	} while (k_uptime_get() < deadline);
	return 0;
}

void modem_tcp_close(void)
{
	at_cmd("AT+QICLOSE=0", 10000);
	socket_closed = true;
}

bool modem_check_shutdown_request(void)
{
	if (NRF_POWER->GPREGRET2 != SHUTDOWN_REQUEST) {
		return false;
	}
	LOG_INF("Debugger asked for a modem shutdown");
	modem_power_off();
	NRF_POWER->GPREGRET2 = SHUTDOWN_DONE;
	return true;
}
