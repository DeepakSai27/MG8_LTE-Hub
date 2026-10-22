/*
 * C10 tag download over BLE, following the ESP32-S3 hub (ble_client_service.c, dhruvaBle.cpp):
 *  0. advertise the hub beacon (Eddystone-UID, namespace AAAABBBBCCCCDDDDAAAA); a tag that hears it
 *     and has records opens its download server and advertises CONFIG_HUB_TAG_NAME ("exactcomms_tag")
 *  1. scan; a tag advertising CONFIG_HUB_TAG_NAME with RSSI >= CONFIG_HUB_TAG_MIN_RSSI is connected
 *  2. MTU exchange, discover service 0x010B: handshake 0x1401, data 0x1402 (config 0x1403 unused)
 *  3. enable indications on handshake, then data (the tag indicates, it does not notify)
 *  4. write the hub's free record count (uint16) to the handshake char
 *  5. the tag notifies its handshake; the hub writes 2 (start) to the data char
 *  6. each data indication becomes one upload queue item; the tag disconnects when done
 */

#pragma once

#include <stdbool.h>

/* Starts scanning for tags. Call once BLE is enabled and the clock is set. */
int tag_client_start(void);

/* A tag is connected or being connected */
bool tag_client_busy(void);
