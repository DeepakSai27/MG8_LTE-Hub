/*
 * C10 tag data parsing and the upload string format. Ported from the ESP32-S3 hub
 * (dhruvaBle.cpp, ble_proxy_utils) so the server receives exactly the same strings.
 *
 * Handshake notification:  "<tag mac>|<fw>|<battery mV hex>|<uptime ms hex>"
 * Data notification:       "<count hex>^<idx hex>-<rssi hex>|<beacon mac>|<start delta ms hex>|<end delta ms hex>,..."
 * Each record becomes two upload strings (contact start and end):
 *   "<rssi>|<battery>|<beacon mac>|<tag mac>|<fw>|<YYYY-mm-dd HH:MM:SS>|<tag uptime d:hh:mm:ss>"
 * and one queue item is the 8 strings of 4 records joined with ','.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define TAG_MAC_SIZE		 18
#define TAG_FIRMWARE_SIZE	 20
#define TAG_RECORDS_PER_PACKET	 4
#define TAG_PACKET_SIZE		 400
#define TAG_QUEUE_ITEM_SIZE	 (TAG_PACKET_SIZE * 8)

struct tag_handshake {
	char mac[TAG_MAC_SIZE];
	int16_t battery;
	char firmware[TAG_FIRMWARE_SIZE + 1];
	int64_t uptime_ms;
};

/* Parses a handshake notification in place. Returns 0, or -EINVAL if it has no '|'. */
int tag_parse_handshake(char *text, struct tag_handshake *hs);

/*
 * Parses a data notification in place and writes the queue item into out.
 * Returns the number of records (0..4), or -EINVAL for a packet without records.
 */
int tag_format_data_packet(char *text, const struct tag_handshake *hs, char *out, size_t out_len);
