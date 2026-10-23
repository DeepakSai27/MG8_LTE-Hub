/*
 * Upload queue and server upload, following the ESP32-S3 hub's remote_data_transfer_component:
 * one TCP connection per queue item, any reply from the server is the ACK, then close. After
 * CONFIG_HUB_SERVER_MAX_RETRIES failures it moves main -> AZ2 -> AZ3 -> main, and returns to
 * the main server CONFIG_HUB_MAIN_SERVER_RETRY_MIN minutes after leaving it.
 */

#pragma once

#include <stdbool.h>

#include <stdint.h>

/* Opens the flash queue (falls back to RAM only without it). Call before tags are downloaded. */
void upload_init(void);

/* Hub identity used in the status ping, "aa:bb:cc:dd:ee:ff" */
void upload_set_hub_id(const char *mac);

/* NRF_POWER->RESETREAS as read at boot, reported in the status ping */
void upload_set_reset_reason(uint32_t resetreas);

/* Queues one item without blocking (safe from the BLE thread). 0 or -ENOMEM when full. */
int upload_queue_put(const char *item);

/* Free tag records the queue can still take, as the hub tells a tag before a download */
int upload_free_records(void);

int upload_items_waiting(void);

/* Uploads the oldest item if there is one. Call from the modem thread. */
void upload_process(void);

/* Queues a status ping (version, MAC, time, uptime, reset reason, counters, signal) */
void upload_queue_status_ping(int signal_dbm);

/* No failed attempt since the last successful upload */
bool upload_last_sync_ok(void);
