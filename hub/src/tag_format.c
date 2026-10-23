#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/logging/log.h>

#include "clock.h"
#include "tag_format.h"

LOG_MODULE_REGISTER(tag_format, LOG_LEVEL_INF);

struct tag_record {
	char data[TAG_RECORD_STRING_SIZE];
	char end_data[TAG_RECORD_STRING_SIZE];
};

static struct tag_record records[TAG_RECORDS_PER_PACKET];

/* Contact time = now - delta, as get_date_time_str_from_unit32_t() on the ESP32 hub */
static void format_contact_time(uint32_t delta_ms, char *buf, size_t len)
{
	clock_format_datetime(clock_now_utc() - delta_ms / 1000, buf, len);
}

/* Tag uptime as "d:hh:mm:ss", as get_time_str_from_long_s() on the ESP32 hub */
static void format_uptime(int64_t ms, char *buf, size_t len)
{
	if (ms < 0) {
		snprintf(buf, len, "Invalid time");
		return;
	}
	int64_t s = ms / 1000;

	snprintf(buf, len, "%lld:%02lld:%02lld:%02lld", s / 86400, (s / 3600) % 24, (s / 60) % 60,
		 s % 60);
}

int tag_parse_handshake(char *text, struct tag_handshake *hs)
{
	char *save;

	if (strchr(text, '|') == NULL) {
		return -EINVAL;
	}
	memset(hs, 0, sizeof(*hs));

	char *item = strtok_r(text, "|", &save);

	for (int i = 0; i < 4 && item != NULL; i++) {
		switch (i) {
		case 0:
			strncpy(hs->mac, item, sizeof(hs->mac) - 1);
			break;
		case 1:
			strncpy(hs->firmware, item, TAG_FIRMWARE_SIZE);
			break;
		case 2:
			hs->battery = (int16_t)strtol(item, NULL, 16);
			break;
		default:
			hs->uptime_ms = strtoll(item, NULL, 16);
			break;
		}
		item = strtok_r(NULL, "|", &save);
	}
	return 0;
}

static void parse_record(char *item, const struct tag_handshake *hs, struct tag_record *rec)
{
	char *save;
	int8_t rssi = 0;
	char beacon[TAG_MAC_SIZE] = "";
	uint32_t start_delta = 0;
	uint32_t end_delta = 0;

	/* "<idx>-<rssi>|<beacon>|<start>|<end>": the index is not uploaded */
	strtok_r(item, "-", &save);
	char *fields = strtok_r(NULL, "-", &save);
	char *field = (fields != NULL) ? strtok_r(fields, "|", &save) : NULL;

	for (int i = 0; i < 4 && field != NULL; i++) {
		switch (i) {
		case 0:
			rssi = (int8_t)strtol(field, NULL, 16);
			break;
		case 1:
			strncpy(beacon, field, sizeof(beacon) - 1);
			break;
		case 2:
			start_delta = (uint32_t)strtoul(field, NULL, 16);
			break;
		default:
			end_delta = (uint32_t)strtoul(field, NULL, 16);
			break;
		}
		field = strtok_r(NULL, "|", &save);
	}

	char start[24];
	char end[24];
	char uptime[32];

	format_contact_time(start_delta, start, sizeof(start));
	format_contact_time(end_delta, end, sizeof(end));
	format_uptime(hs->uptime_ms, uptime, sizeof(uptime));

	if (IS_ENABLED(CONFIG_HUB_LOG_RECORDS)) {
		LOG_INF("Record tag %s beacon %s rssi %d start %s end %s", hs->mac, beacon, rssi,
			start, end);
	}

	snprintf(rec->data, sizeof(rec->data), "%d|%d|%s|%s|%s|%s|%s", rssi, hs->battery, beacon,
		 hs->mac, hs->firmware, start, uptime);
	snprintf(rec->end_data, sizeof(rec->end_data), "%d|%d|%s|%s|%s|%s|%s", rssi, hs->battery,
		 beacon, hs->mac, hs->firmware, end, uptime);
}

int tag_format_data_packet(char *text, const struct tag_handshake *hs, char *out, size_t out_len)
{
	char *save;
	char *items[TAG_RECORDS_PER_PACKET];
	int count = 0;

	if (strchr(text, '-') == NULL) {
		return -EINVAL;
	}
	memset(records, 0, sizeof(records));

	char *total_str = strtok_r(text, "^", &save);
	int total = (total_str != NULL) ? (int)strtol(total_str, NULL, 16) : 0;
	char *list = strtok_r(NULL, "^", &save);
	char *item = (list != NULL) ? strtok_r(list, ",", &save) : NULL;

	while (item != NULL && count < total && count < TAG_RECORDS_PER_PACKET) {
		items[count++] = item;
		item = strtok_r(NULL, ",", &save);
	}
	for (int i = 0; i < count; i++) {
		parse_record(items[i], hs, &records[i]);
	}

	/* Always 8 fields, empty for missing records, exactly as the ESP32 hub queues them */
	snprintf(out, out_len, "%s,%s,%s,%s,%s,%s,%s,%s", records[0].data, records[0].end_data,
		 records[1].data, records[1].end_data, records[2].data, records[2].end_data,
		 records[3].data, records[3].end_data);
	return count;
}
