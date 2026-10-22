/*
 * Minew MG8 bring-up: powers up the Quectel modem and queries it over the main UART.
 *
 * Pins (full map in README.md):
 *   LEDs (active low): red P0.08, green P0.06, blue P0.12
 *   Button (active low, needs pull-up): P0.04
 *   Modem 3.8 V buck enable: P1.09 (high = on)
 *   Modem PWRKEY: P0.15, drives an N-MOSFET that pulls PWRKEY low (high = key pressed)
 *   Modem RESET_N: P0.13, same arrangement (high = reset held). Keep low.
 *   Modem W_DISABLE#: P0.31, same arrangement (high = airplane mode). Keep low.
 *   UART through a level shifter whose OE follows the modem's VDD_EXT, so the lines only work
 *   while the modem is on: nRF TX P0.14 -> modem RXD, modem TXD -> nRF RX P0.21.
 *   DTR P0.16 and RTS P0.24 are modem inputs, held low (awake, ready to receive).
 *   P0.13 / P0.15 are also the T5 / T6 test pads.
 *
 * All output goes to SEGGER RTT (channel 0). After the start-up query the firmware becomes an AT
 * console: a line typed into RTT channel 0 (J-Link RTT Viewer, or telnet localhost 19021) is sent
 * to the modem, and everything the modem prints, including URCs, is copied back to RTT.
 * Press the button to power-cycle and query again.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <SEGGER_RTT.h>
#include <hal/nrf_gpio.h>
#include <nrf.h>

#define PIN(port, n) NRF_GPIO_PIN_MAP(port, n)

#define LED_RED	  PIN(0, 8)
#define LED_GREEN PIN(0, 6)
#define LED_BLUE  PIN(0, 12)
#define BUTTON	  PIN(0, 4)

#define MODEM_PWR_EN	PIN(1, 9)
#define MODEM_PWRKEY	PIN(0, 15)
#define MODEM_RESET	PIN(0, 13)
#define MODEM_W_DISABLE PIN(0, 31)
#define MODEM_DTR	PIN(0, 16)
#define MODEM_RTS	PIN(0, 24)
#define UART_TX		PIN(0, 14)
#define UART_RX		PIN(0, 21)

#define SUPPLY_SETTLE_MS 100
#define PWRKEY_PULSE_MS	 1000
#define MODEM_BOOT_WAIT_S 20
#define AT_REPLY_WAIT_MS 300

static uint8_t uart_tx_buf[32];
static uint8_t uart_rx_buf[512];
static char reply[512];

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

static void output_low(uint32_t pin)
{
	nrf_gpio_pin_clear(pin);
	nrf_gpio_cfg_output(pin);
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

	/* Modem off, PWRKEY released, not in reset, not in airplane mode */
	output_low(MODEM_PWR_EN);
	output_low(MODEM_PWRKEY);
	output_low(MODEM_RESET);
	output_low(MODEM_W_DISABLE);
	output_low(MODEM_DTR);
	output_low(MODEM_RTS);
	nrf_gpio_cfg_input(UART_RX, NRF_GPIO_PIN_NOPULL);
}

/**
 * Sends a command on UARTE1 and collects the reply for wait_ms.
 * The reply is copied into the global reply buffer with non-printable bytes replaced.
 */
static size_t uart_exchange(const char *cmd, int wait_ms)
{
	NRF_UARTE_Type *uarte = NRF_UARTE1;
	size_t len = MIN(strlen(cmd), sizeof(uart_tx_buf));

	/* The UARTE drives TX itself once enabled; setting the output level first avoids a start glitch */
	nrf_gpio_pin_set(UART_TX);
	nrf_gpio_cfg_output(UART_TX);

	uarte->ENABLE = UARTE_ENABLE_ENABLE_Disabled;
	uarte->PSEL.TXD = UART_TX;
	uarte->PSEL.RXD = UART_RX;
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

	memcpy(uart_tx_buf, cmd, len);
	uarte->TXD.PTR = (uint32_t)uart_tx_buf;
	uarte->TXD.MAXCNT = len;
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

static bool at_ok(void)
{
	/* The first AT also lets an auto-bauding modem lock onto the baud rate */
	uart_exchange("AT\r", 100);
	uart_exchange("AT\r", AT_REPLY_WAIT_MS);
	return strstr(reply, "OK") != NULL;
}

static void modem_command(const char *cmd, int wait_ms)
{
	uart_exchange(cmd, wait_ms);
	printk(">> %s\n%s\n", cmd, reply);
}

static void modem_query(void)
{
	printk("\n--- Modem information ---\n");
	modem_command("ATE0\r", 300);
	/* Verbose errors, and registration reports that include the reject cause */
	modem_command("AT+CMEE=2\r", 300);
	modem_command("AT+CEREG=3\r", 300);
	modem_command("ATI\r", 500);
	modem_command("AT+CGMR\r", 500);
	modem_command("AT+IPR?\r", 300);
	modem_command("AT+QCCID\r", 500);
	modem_command("AT+CPIN?\r", 500);
	modem_command("AT+CSQ\r", 500);
	modem_command("AT+CEREG?\r", 500);
	modem_command("AT+COPS?\r", 1000);
}

/* Polls signal and LTE registration until the modem registers (home or roaming) or time runs out */
static void modem_wait_registered(int seconds)
{
	printk("\n--- Waiting up to %d s for network registration ---\n", seconds);
	for (int s = 0; s < seconds; s += 10) {
		uart_exchange("AT+CSQ\r", 300);
		printk("t+%3ds %s", s, strtok(reply, "\r\n") ?: "");
		uart_exchange("AT+CEREG?\r", 300);
		char *line = strtok(reply, "\r\n");

		printk("  %s\n", line ?: "");
		/* +CEREG: <n>,<stat>[,...]; stat 1 = home, 5 = roaming */
		char *stat = (line != NULL) ? strchr(line, ',') : NULL;

		if (stat != NULL && (stat[1] == '1' || stat[1] == '5') &&
		    (stat[2] == ',' || stat[2] == '\0')) {
			modem_command("AT+COPS?\r", 1000);
			modem_command("AT+QNWINFO\r", 500);
			modem_command("AT+QCSQ\r", 500);
			return;
		}
		k_msleep(10000 - 600);
	}
	printk("Not registered after %d s\n", seconds);
	modem_command("AT+QNWINFO\r", 500);
	modem_command("AT+QCFG=\"band\"\r", 500);
}

/* Returns the seconds the modem took to answer AT, or -1 if it never did */
static int modem_wait_ready(int seconds)
{
	for (int s = 1; s <= seconds; s++) {
		if (at_ok()) {
			return s;
		}
		if (reply[0] != '\0') {
			printk("  t+%ds: %s\n", s, reply);
		}
		k_msleep(1000 - 100 - AT_REPLY_WAIT_MS);
	}
	return -1;
}

static void modem_power_off(void)
{
	/* Cutting the supply under a running modem can corrupt its file system, so shut it down first */
	if (nrf_gpio_pin_out_read(MODEM_PWR_EN) && at_ok()) {
		printk("Shutting the modem down (AT+QPOWD)\n");
		modem_command("AT+QPOWD=1\r", 5000);
	}
	nrf_gpio_pin_clear(MODEM_PWRKEY);
	nrf_gpio_pin_clear(MODEM_PWR_EN);
}

static bool modem_power_on(void)
{
	printk("\n=== Modem power-up ===\n");
	led_set(false, false, true);

	modem_power_off();
	k_msleep(1000);

	printk("3.8 V enable (P1.09) on\n");
	nrf_gpio_pin_set(MODEM_PWR_EN);
	k_msleep(SUPPLY_SETTLE_MS);

	printk("PWRKEY (P0.15) pressed for %d ms\n", PWRKEY_PULSE_MS);
	nrf_gpio_pin_set(MODEM_PWRKEY);
	k_msleep(PWRKEY_PULSE_MS);
	nrf_gpio_pin_clear(MODEM_PWRKEY);

	printk("Waiting up to %d s for AT on P0.14/P0.21 at 115200\n", MODEM_BOOT_WAIT_S);
	int s = modem_wait_ready(MODEM_BOOT_WAIT_S);

	if (s < 0) {
		led_set(true, false, false);
		printk("No answer from the modem. Press the button to try again.\n");
		return false;
	}
	printk("Modem answered after %d s\n", s);
	led_set(false, true, false);
	return true;
}

/*
 * The console uses the legacy UART0 (no EasyDMA) so every received byte can be picked up as it
 * arrives. Its RX FIFO holds 6 bytes (~0.5 ms at 115200), so the main loop polls without sleeping.
 * UARTE1 (uart_exchange) uses the same pins, so the console is stopped while it runs.
 */
static void console_start(void)
{
	nrf_gpio_pin_set(UART_TX);
	nrf_gpio_cfg_output(UART_TX);
	nrf_gpio_cfg_input(UART_RX, NRF_GPIO_PIN_NOPULL);

	NRF_UART0->ENABLE = UART_ENABLE_ENABLE_Disabled;
	NRF_UART0->PSEL.TXD = UART_TX;
	NRF_UART0->PSEL.RXD = UART_RX;
	NRF_UART0->PSEL.RTS = UINT32_MAX;
	NRF_UART0->PSEL.CTS = UINT32_MAX;
	NRF_UART0->BAUDRATE = UART_BAUDRATE_BAUDRATE_Baud115200;
	NRF_UART0->CONFIG = 0;
	NRF_UART0->ENABLE = UART_ENABLE_ENABLE_Enabled;
	NRF_UART0->EVENTS_RXDRDY = 0;
	NRF_UART0->EVENTS_TXDRDY = 0;
	NRF_UART0->TASKS_STARTRX = 1;
	NRF_UART0->TASKS_STARTTX = 1;
}

static void console_stop(void)
{
	NRF_UART0->TASKS_STOPRX = 1;
	NRF_UART0->TASKS_STOPTX = 1;
	NRF_UART0->ENABLE = UART_ENABLE_ENABLE_Disabled;
	NRF_UART0->PSEL.TXD = UINT32_MAX;
	NRF_UART0->PSEL.RXD = UINT32_MAX;
}

static char modem_line[256];
static size_t modem_line_len;

static void console_flush(void)
{
	if (modem_line_len > 0) {
		SEGGER_RTT_Write(0, modem_line, modem_line_len);
		modem_line_len = 0;
	}
}

/* Copies modem output to RTT, a line at a time so RTT writes stay cheap */
static void console_receive(void)
{
	if (NRF_UART0->ERRORSRC) {
		NRF_UART0->ERRORSRC = NRF_UART0->ERRORSRC;
		console_flush();
		printk("[uart error]\n");
	}
	while (NRF_UART0->EVENTS_RXDRDY) {
		NRF_UART0->EVENTS_RXDRDY = 0;
		char c = NRF_UART0->RXD;

		if (c == '\r') {
			continue;
		}
		modem_line[modem_line_len++] = c;
		if (c == '\n' || modem_line_len == sizeof(modem_line)) {
			console_flush();
		}
	}
}

static void console_send(const char *line, size_t len)
{
	console_flush();
	printk("> %.*s\n", (int)len, line);
	for (size_t i = 0; i <= len; i++) {
		NRF_UART0->EVENTS_TXDRDY = 0;
		NRF_UART0->TXD = (i < len) ? line[i] : '\r';
		while (!NRF_UART0->EVENTS_TXDRDY) {
			console_receive();
		}
	}
}

static void console_run(void)
{
	static char line[128];
	size_t len = 0;
	uint32_t idle = 0;

	printk("\n--- AT console: type a command into RTT channel 0 ---\n");
	console_start();
	for (;;) {
		console_receive();

		char in[16];
		unsigned int n = SEGGER_RTT_Read(0, in, sizeof(in));

		for (unsigned int i = 0; i < n; i++) {
			if (in[i] == '\r' || in[i] == '\n') {
				if (len > 0) {
					console_send(line, len);
					len = 0;
				}
			} else if (len < sizeof(line)) {
				line[len++] = in[i];
			}
		}

		/* A reply without a trailing newline still shows up after ~50 ms of quiet */
		if (modem_line_len > 0 && ++idle > 5000) {
			console_flush();
		}
		if (modem_line_len == 0) {
			idle = 0;
		}

		if (!nrf_gpio_pin_read(BUTTON)) {
			console_stop();
			k_msleep(50);
			while (!nrf_gpio_pin_read(BUTTON)) {
				k_msleep(10);
			}
			if (modem_power_on()) {
				modem_query();
				modem_wait_registered(120);
			}
			printk("\n--- AT console ---\n");
			console_start();
		}
		k_busy_wait(10);
	}
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
	printk("RESETREAS 0x%08x, UICR REGOUT0 0x%08x, APPROTECT 0x%08x\n", reset_reason,
	       NRF_UICR->REGOUT0, NRF_UICR->APPROTECT);

	if (modem_power_on()) {
		modem_query();
		modem_wait_registered(120);
	}

	console_run();
	return 0;
}
