#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf.h>

#include "clock.h"
#include "modem.h"
#include "store.h"
#include "tag_format.h"
#include "upload.h"

LOG_MODULE_REGISTER(upload, LOG_LEVEL_INF);

/* New items land here; with the flash queue on, the store thread moves them into flash */
K_MSGQ_DEFINE(upload_queue, TAG_QUEUE_ITEM_SIZE, CONFIG_HUB_QUEUE_SIZE, 4);

static bool flash_queue;
static K_THREAD_STACK_DEFINE(store_stack, 1536);
static struct k_thread store_thread;

enum server { MAIN_SERVER, AZ2_SERVER, AZ3_SERVER };

static const char *const servers[] = {
	[MAIN_SERVER] = CONFIG_HUB_SERVER_MAIN,
	[AZ2_SERVER] = CONFIG_HUB_SERVER_AZ2,
	[AZ3_SERVER] = CONFIG_HUB_SERVER_AZ3,
};

static enum server current_server = MAIN_SERVER;
static int64_t backup_since_ms;
static int retry_attempts;
static uint32_t ok_count;
static uint32_t fail_count;
static char hub_id[18] = "00:00:00:00:00:00";
static uint32_t boot_reset_reason;

/* One extra byte for the optional newline and one for the terminator */
static char item[TAG_QUEUE_ITEM_SIZE + 2];

void upload_set_hub_id(const char *mac)
{
	strncpy(hub_id, mac, sizeof(hub_id) - 1);
}

int upload_queue_put(const char *data)
{
	static char buf[TAG_QUEUE_ITEM_SIZE];

	strncpy(buf, data, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	if (k_msgq_put(&upload_queue, buf, K_NO_WAIT) != 0) {
		LOG_ERR("Upload queue full, dropping an item");
		return -ENOMEM;
	}
	return 0;
}

/* Moves items from RAM to flash, oldest first. Flash erases (~55 ms) stay out of the BLE thread. */
static void store_loop(void *p1, void *p2, void *p3)
{
	static char buf[TAG_QUEUE_ITEM_SIZE];

	for (;;) {
		k_msgq_get(&upload_queue, buf, K_FOREVER);
		int err;

		while ((err = store_put(buf)) != 0) {
			LOG_WRN("Flash queue %s, retrying", err == -ENOMEM ? "full" : "write failed");
			k_sleep(K_SECONDS(10));
		}
	}
}

void upload_init(void)
{
	if (!IS_ENABLED(CONFIG_HUB_FLASH_QUEUE) || store_init() != 0) {
		LOG_WRN("Upload queue in RAM only (%d items)", CONFIG_HUB_QUEUE_SIZE);
		return;
	}
	flash_queue = true;
	k_thread_create(&store_thread, store_stack, K_THREAD_STACK_SIZEOF(store_stack), store_loop,
			NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&store_thread, "store");
}

int upload_free_records(void)
{
	int items = k_msgq_num_free_get(&upload_queue);

	if (flash_queue) {
		items = MIN(items, store_free() - (int)k_msgq_num_used_get(&upload_queue));
	}
	return MAX(items, 0) * TAG_RECORDS_PER_PACKET;
}

int upload_items_waiting(void)
{
	return k_msgq_num_used_get(&upload_queue) + (flash_queue ? store_count() : 0);
}

bool upload_last_sync_ok(void)
{
	return retry_attempts == 0;
}

static void record_failure(void)
{
	fail_count++;
	retry_attempts++;
	if (retry_attempts <= CONFIG_HUB_SERVER_MAX_RETRIES) {
		return;
	}
	retry_attempts = 1;
	if (current_server == MAIN_SERVER) {
		current_server = AZ2_SERVER;
		backup_since_ms = k_uptime_get();
	} else if (current_server == AZ2_SERVER) {
		current_server = AZ3_SERVER;
		backup_since_ms = k_uptime_get();
	} else {
		current_server = MAIN_SERVER;
		backup_since_ms = 0;
	}
	LOG_WRN("Too many failures, switching to server %s", servers[current_server]);
}

void upload_process(void)
{
	if (flash_queue ? store_peek(item, TAG_QUEUE_ITEM_SIZE) < 0
			: k_msgq_peek(&upload_queue, item) != 0) {
		return;
	}

	if (backup_since_ms != 0 &&
	    k_uptime_get() - backup_since_ms > CONFIG_HUB_MAIN_SERVER_RETRY_MIN * 60 * 1000LL) {
		LOG_INF("Trying the main server again");
		current_server = MAIN_SERVER;
		backup_since_ms = 0;
	}

	const char *host = servers[current_server];
	size_t len = strlen(item);

	if (IS_ENABLED(CONFIG_HUB_UPLOAD_APPEND_NEWLINE)) {
		item[len++] = '\n';
		item[len] = '\0';
	}

	LOG_INF("Uploading %u bytes to %s:%d (%d waiting)", len, host, CONFIG_HUB_SERVER_PORT,
		upload_items_waiting());

	int err = modem_tcp_open(host, CONFIG_HUB_SERVER_PORT);

	if (err == 0) {
		err = modem_tcp_send(item, len);
	}
	if (err == 0) {
		int n = modem_tcp_wait_reply(CONFIG_HUB_ACK_TIMEOUT_S * 1000);

		err = (n > 0) ? 0 : (n == 0 ? -ETIMEDOUT : n);
	}
	modem_tcp_close();

	if (err == 0) {
		if (flash_queue) {
			store_pop();
		} else {
			k_msgq_get(&upload_queue, item, K_NO_WAIT);
		}
		ok_count++;
		retry_attempts = 0;
		LOG_INF("ACK OK (ok %u, failed %u)", ok_count, fail_count);
	} else {
		record_failure();
		LOG_WRN("Upload failed (%d), item kept (ok %u, failed %u, retry %d)", err, ok_count,
			fail_count, retry_attempts);
	}
}

static const char *reset_reason(uint32_t r)
{
	if (r & POWER_RESETREAS_RESETPIN_Msk) {
		return "Reset by external pin";
	}
	if (r & POWER_RESETREAS_DOG_Msk) {
		return "Reset due to watchdog";
	}
	if (r & POWER_RESETREAS_SREQ_Msk) {
		return "Software reset";
	}
	if (r & POWER_RESETREAS_LOCKUP_Msk) {
		return "Reset due to CPU lockup";
	}
	if (r & (POWER_RESETREAS_DIF_Msk | POWER_RESETREAS_OFF_Msk)) {
		return "Reset from debugger or wake-up";
	}
	return "Reset due to power-on event";
}

void upload_set_reset_reason(uint32_t resetreas)
{
	boot_reset_reason = resetreas;
}

void upload_queue_status_ping(int signal_dbm)
{
	char now[24];
	char ping[TAG_QUEUE_ITEM_SIZE];

	clock_format_datetime(clock_now_utc(), now, sizeof(now));

	/* Uptime as "h:m:s.ms", like get_time_str_from_long() on the ESP32 hub */
	int64_t ms = k_uptime_get();
	char uptime[32];

	snprintf(uptime, sizeof(uptime), "%lld:%lld:%lld.%lld", ms / 3600000, (ms / 60000) % 60,
		 (ms / 1000) % 60, ms % 1000);

	snprintf(ping, sizeof(ping), "%s|%s|%s|%s|%s|%u|%u|%d", CONFIG_HUB_VERSION, hub_id, now,
		 uptime, reset_reason(boot_reset_reason), ok_count, fail_count, signal_dbm);
	LOG_INF("Status ping: %s", ping);

	if (upload_queue_put(ping) == 0) {
		ok_count = 0;
		fail_count = 0;
	}
}
