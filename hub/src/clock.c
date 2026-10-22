#include <stdio.h>
#include <zephyr/kernel.h>

#include "clock.h"

static int64_t base_epoch_s;
static int64_t base_uptime_ms;
static bool is_set;

void clock_set_utc(int64_t epoch_s)
{
	base_uptime_ms = k_uptime_get();
	base_epoch_s = epoch_s;
	is_set = true;
}

bool clock_is_set(void)
{
	return is_set;
}

int64_t clock_now_utc(void)
{
	return base_epoch_s + (k_uptime_get() - base_uptime_ms) / 1000;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil) */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 719468;
}

int64_t clock_epoch_from_civil(int year, int month, int day, int hour, int min, int sec)
{
	return days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
}

void clock_format_datetime(int64_t epoch_s, char *buf, size_t len)
{
	int64_t days = epoch_s / 86400;
	int64_t rem = epoch_s % 86400;

	if (rem < 0) {
		rem += 86400;
		days--;
	}

	/* civil_from_days */
	days += 719468;
	int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	int64_t doe = days - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int d = (int)(doy - (153 * mp + 2) / 5 + 1);
	int m = (int)(mp < 10 ? mp + 3 : mp - 9);
	int y = (int)(yoe + era * 400 + (m <= 2));

	snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, (int)(rem / 3600),
		 (int)(rem / 60 % 60), (int)(rem % 60));
}
