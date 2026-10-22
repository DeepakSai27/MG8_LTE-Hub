/* Wall clock for the hub: set once from the modem (NTP), then kept from the uptime counter */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void clock_set_utc(int64_t epoch_s);
bool clock_is_set(void);
int64_t clock_now_utc(void);

/* "YYYY-mm-dd HH:MM:SS" (UTC) for an epoch, like the ESP32 hub with TZ=UTC. buf >= 20 bytes. */
void clock_format_datetime(int64_t epoch_s, char *buf, size_t len);

/* Epoch for a civil UTC date and time */
int64_t clock_epoch_from_civil(int year, int month, int day, int hour, int min, int sec);
