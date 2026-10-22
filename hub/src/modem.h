/*
 * Quectel EG916Q-GL on the MG8: power control, AT commands over uart1, network registration,
 * time and one TCP socket through the modem's own IP stack (AT+QIOPEN / QISEND / QIRD).
 * All calls block and are serialised; call them from one thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int modem_init(void);

/* Powers the modem up (3.8 V on, PWRKEY pulse) and waits for AT. 0 or -ETIMEDOUT. */
int modem_power_on(void);

/* AT+QPOWD if the modem answers, then switches its supply off */
void modem_power_off(void);

bool modem_is_on(void);

/* Three AT commands in a row timed out: the modem needs a power cycle */
bool modem_is_stuck(void);

/* Registered (home or roaming) and the PDP context is active. Activates the context if needed. */
bool modem_network_ready(void);

/* Signal in dBm from AT+CSQ, or 0 if unknown */
int modem_signal_dbm(void);

/* Gets UTC from NTP (or the network time as a fallback) and sets the hub clock. 0 on success. */
int modem_sync_time(void);

/* TCP socket 0 */
int modem_tcp_open(const char *host, uint16_t port);
int modem_tcp_send(const void *data, size_t len);
/* Waits for any reply. Returns the number of bytes, 0 on timeout, -ENOTCONN if closed. */
int modem_tcp_wait_reply(int timeout_ms);
void modem_tcp_close(void);

/*
 * Debugger mailbox (GPREGRET2), same as the bring-up firmware: 0xA5 = shut the modem down,
 * answered with 0x5A. Call regularly; returns true once a shutdown has been done.
 */
bool modem_check_shutdown_request(void);
