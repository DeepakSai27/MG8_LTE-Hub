#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "tag_client.h"
#include "tag_format.h"
#include "upload.h"

LOG_MODULE_REGISTER(tag_client, LOG_LEVEL_INF);

#define DATA_TRANSFER_SERVICE_UUID 0x010B
#define HANDSHAKE_CHAR_UUID	   0x1401
#define DATA_TRANSFER_CHAR_UUID	   0x1402

/* Values written to the data char */
#define HUB_DATA_TRANSFER_START 2
#define HUB_DATA_TRANSFER_STOP	0xFFFF /* -1 as uint16, as the ESP32 hub writes it */

/* A download that has not finished by then is dropped */
#define CONNECTION_TIMEOUT K_SECONDS(60)

static struct bt_conn *conn;
static uint16_t svc_end;
static uint16_t handshake_handle;
static uint16_t data_handle;
static bool handshake_ok;
static struct tag_handshake handshake;
static int packets;
static int records;
static bool started;

static struct bt_gatt_exchange_params mtu_params;
static struct bt_gatt_discover_params disc_params;
static struct bt_gatt_discover_params ccc_disc_params;
static struct bt_gatt_subscribe_params hs_sub;
static struct bt_gatt_subscribe_params data_sub;

static char notify_buf[TAG_PACKET_SIZE];
static char item[TAG_QUEUE_ITEM_SIZE];

static void start_scan(void);
static void timeout_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_handler);

static void write_u16(uint16_t handle, uint16_t value)
{
	uint8_t buf[2];

	sys_put_le16(value, buf);
	int err = bt_gatt_write_without_response(conn, handle, buf, sizeof(buf), false);

	if (err) {
		LOG_WRN("Write to handle 0x%04x failed: %d", handle, err);
	}
}

static void stop_and_disconnect(const char *why)
{
	LOG_WRN("Dropping the tag: %s", why);
	if (data_handle) {
		write_u16(data_handle, HUB_DATA_TRANSFER_STOP);
	}
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void timeout_handler(struct k_work *work)
{
	if (conn != NULL) {
		stop_and_disconnect("download timed out");
	}
}

/*
 * The tag sends handshake and data as indications (the ESP32 hub writes 2 to the CCC) and only
 * sends the next packet after the hub confirms the previous one; Zephyr confirms on return.
 */
static uint8_t notify_cb(struct bt_conn *c, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	if (data == NULL) {
		params->value_handle = 0;
		return BT_GATT_ITER_STOP;
	}

	size_t len = MIN(length, sizeof(notify_buf) - 1);

	memcpy(notify_buf, data, len);
	notify_buf[len] = '\0';

	if (params->value_handle == handshake_handle) {
		if (tag_parse_handshake(notify_buf, &handshake) != 0) {
			LOG_DBG("Empty handshake, ignored");
			return BT_GATT_ITER_CONTINUE;
		}
		handshake_ok = true;
		LOG_INF("Tag handshake: mac %s, fw %s, %d mV, up %lld ms", handshake.mac,
			handshake.firmware, handshake.battery, handshake.uptime_ms);
		write_u16(data_handle, HUB_DATA_TRANSFER_START);
	} else if (params->value_handle == data_handle) {
		if (!handshake_ok) {
			stop_and_disconnect("data before handshake");
			return BT_GATT_ITER_STOP;
		}
		int n = tag_format_data_packet(notify_buf, &handshake, item, sizeof(item));

		if (n < 0) {
			LOG_DBG("Empty data packet, ignored");
			return BT_GATT_ITER_CONTINUE;
		}
		packets++;
		records += n;
		LOG_INF("Packet %d: %d records", packets, n);
		LOG_DBG("%s", item);
		upload_queue_put(item);
	}
	return BT_GATT_ITER_CONTINUE;
}

static void data_subscribed(struct bt_conn *c, uint8_t err, struct bt_gatt_subscribe_params *p)
{
	if (err) {
		stop_and_disconnect("could not enable data indications");
		return;
	}
	uint16_t free_records = upload_free_records();

	LOG_INF("Indications on, offering %u records", free_records);
	write_u16(handshake_handle, free_records);
}

static void handshake_subscribed(struct bt_conn *c, uint8_t err,
				 struct bt_gatt_subscribe_params *p)
{
	if (err) {
		stop_and_disconnect("could not enable handshake indications");
		return;
	}
	data_sub.notify = notify_cb;
	data_sub.subscribe = data_subscribed;
	data_sub.value = BT_GATT_CCC_INDICATE;
	data_sub.value_handle = data_handle;
	data_sub.ccc_handle = 0; /* discovered automatically */
	data_sub.end_handle = svc_end;
	data_sub.disc_params = &ccc_disc_params;
	int e = bt_gatt_subscribe(conn, &data_sub);

	if (e && e != -EALREADY) {
		stop_and_disconnect("subscribe to data failed");
	}
}

static uint8_t discover_cb(struct bt_conn *c, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	if (attr == NULL) {
		if (params->type == BT_GATT_DISCOVER_PRIMARY) {
			stop_and_disconnect("no data transfer service");
			return BT_GATT_ITER_STOP;
		}
		if (handshake_handle == 0 || data_handle == 0) {
			stop_and_disconnect("handshake or data char missing (old tag firmware?)");
			return BT_GATT_ITER_STOP;
		}
		hs_sub.notify = notify_cb;
		hs_sub.subscribe = handshake_subscribed;
		hs_sub.value = BT_GATT_CCC_INDICATE;
		hs_sub.value_handle = handshake_handle;
		hs_sub.ccc_handle = 0;
		hs_sub.end_handle = svc_end;
		hs_sub.disc_params = &ccc_disc_params;
		int e = bt_gatt_subscribe(conn, &hs_sub);

		if (e && e != -EALREADY) {
			stop_and_disconnect("subscribe to handshake failed");
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		const struct bt_gatt_service_val *svc = attr->user_data;

		svc_end = svc->end_handle;
		params->uuid = NULL;
		params->start_handle = attr->handle + 1;
		params->end_handle = svc_end;
		params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
		if (bt_gatt_discover(c, params)) {
			stop_and_disconnect("characteristic discovery failed");
		}
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_chrc *chrc = attr->user_data;

	if (chrc->uuid->type == BT_UUID_TYPE_16) {
		uint16_t uuid = BT_UUID_16(chrc->uuid)->val;

		if (uuid == HANDSHAKE_CHAR_UUID) {
			handshake_handle = chrc->value_handle;
		} else if (uuid == DATA_TRANSFER_CHAR_UUID) {
			data_handle = chrc->value_handle;
		}
	}
	return BT_GATT_ITER_CONTINUE;
}

static void mtu_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_exchange_params *params)
{
	static struct bt_uuid_16 svc_uuid = BT_UUID_INIT_16(DATA_TRANSFER_SERVICE_UUID);

	LOG_INF("MTU %u%s", bt_gatt_get_mtu(c), err ? " (exchange failed)" : "");

	disc_params.uuid = &svc_uuid.uuid;
	disc_params.func = discover_cb;
	disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc_params.type = BT_GATT_DISCOVER_PRIMARY;
	if (bt_gatt_discover(c, &disc_params)) {
		stop_and_disconnect("service discovery failed");
	}
}

static void connected(struct bt_conn *c, uint8_t err)
{
	if (c != conn) {
		return;
	}
	if (err) {
		LOG_WRN("Connecting to the tag failed: 0x%02x", err);
		bt_conn_unref(conn);
		conn = NULL;
		start_scan();
		return;
	}
	LOG_INF("Tag connected");
	svc_end = handshake_handle = data_handle = 0;
	handshake_ok = false;
	packets = records = 0;
	k_work_reschedule(&timeout_work, CONNECTION_TIMEOUT);

	mtu_params.func = mtu_cb;
	if (bt_gatt_exchange_mtu(c, &mtu_params)) {
		mtu_cb(c, 1, &mtu_params);
	}
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
	if (c != conn) {
		return;
	}
	LOG_INF("Tag disconnected (0x%02x): %d packets, %d records, %d items waiting", reason,
		packets, records, upload_items_waiting());
	k_work_cancel_delayable(&timeout_work);
	bt_conn_unref(conn);
	conn = NULL;
	start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static bool find_name(struct bt_data *data, void *user_data)
{
	char *name = user_data;

	if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
		size_t len = MIN(data->data_len, 31);

		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	}
	return true;
}

#if defined(CONFIG_HUB_LOG_NEARBY)
/* Eddystone service data (UUID 0xFEAA), how the hub code expects a tag outside download mode */
static bool find_eddystone(struct bt_data *data, void *user_data)
{
	bool *found = user_data;

	if (data->type == BT_DATA_SVC_DATA16 && data->data_len >= 2 &&
	    sys_get_le16(data->data) == 0xFEAA) {
		*found = true;
		return false;
	}
	return true;
}

/* Logs each named or Eddystone device once, to find out what a tag actually advertises */
static void log_nearby(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		       struct net_buf_simple *ad, const char *name)
{
	static bt_addr_le_t seen[64];
	static size_t seen_count;
	bool eddystone = false;
	struct net_buf_simple_state state;

	net_buf_simple_save(ad, &state);
	bt_data_parse(ad, find_eddystone, &eddystone);
	net_buf_simple_restore(ad, &state);

	/* A watched address is logged with its raw advertising data, every 5 s at most */
	char addr_str[BT_ADDR_LE_STR_LEN];
	static int64_t last_watch_log;

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	if (strlen(CONFIG_HUB_WATCH_ADDR_PREFIX) > 0 &&
	    strncasecmp(addr_str, CONFIG_HUB_WATCH_ADDR_PREFIX,
			strlen(CONFIG_HUB_WATCH_ADDR_PREFIX)) == 0 &&
	    k_uptime_get() - last_watch_log > 5000) {
		last_watch_log = k_uptime_get();
		LOG_INF("Watched %s RSSI %d type %u name \"%s\"", addr_str, rssi, type, name);
		LOG_HEXDUMP_INF(ad->data, ad->len, "advertising data");
	}

	bool log_all = strcmp(CONFIG_HUB_WATCH_ADDR_PREFIX, "*") == 0;

	if (name[0] == '\0' && !eddystone && !log_all) {
		return;
	}
	for (size_t i = 0; i < seen_count; i++) {
		if (bt_addr_le_eq(&seen[i], addr)) {
			return;
		}
	}
	if (seen_count < ARRAY_SIZE(seen)) {
		bt_addr_le_copy(&seen[seen_count++], addr);
	}

	LOG_INF("Nearby: %s RSSI %d type %u name \"%s\"%s", addr_str, rssi, type, name,
		eddystone ? " [Eddystone]" : "");
}
#endif

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		    struct net_buf_simple *ad)
{
	static int64_t last_far_log;
	char name[32] = "";
	struct net_buf_simple_state state;

	net_buf_simple_save(ad, &state);
	bt_data_parse(ad, find_name, name);
	net_buf_simple_restore(ad, &state);

#if defined(CONFIG_HUB_LOG_NEARBY)
	log_nearby(addr, rssi, type, ad, name);
#endif

	if (conn != NULL || (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_SCAN_RSP)) {
		return;
	}
	if (strcmp(name, CONFIG_HUB_TAG_NAME) != 0) {
		return;
	}

	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	if (rssi < CONFIG_HUB_TAG_MIN_RSSI) {
		if (k_uptime_get() - last_far_log > 10000) {
			LOG_INF("Tag %s too far (RSSI %d)", addr_str, rssi);
			last_far_log = k_uptime_get();
		}
		return;
	}

	LOG_INF("Tag %s found, RSSI %d, connecting", addr_str, rssi);
	bt_le_scan_stop();
	bt_le_adv_stop();
	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &conn);

	if (err) {
		LOG_WRN("Create connection failed: %d", err);
		conn = NULL;
		start_scan();
	}
}

/*
 * Hub beacon: the tag only opens its download server after it hears this Eddystone-UID frame
 * (namespace AAAA BBBB CCCC DDDD AAAA). Same bytes as adv_raw_data in the ESP32 hub. The tag's
 * scanner filters on the 0xFEAA UUID list and needs exactly 20 bytes of service data.
 */
static const struct bt_data hub_beacon[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0xAA, 0xFE),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16,
		      0xAA, 0xFE,				  /* Eddystone */
		      0x00,					  /* frame type UID */
		      0xE8,					  /* TX power at 0 m */
		      0xAA, 0xAA, 0xBB, 0xBB, 0xCC, 0xCC, 0xDD, 0xDD, 0xAA, 0xAA, /* hub namespace */
		      0x01, 0x02, 0x03, 0x04, 0x05, 0x06),	  /* instance */
};

static void start_beacon(void)
{
	/* 20-40 ms like the ESP32 hub; non-connectable, so no peripheral role is needed */
	const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY, 0x20, 0x40, NULL);
	int err = bt_le_adv_start(&param, hub_beacon, ARRAY_SIZE(hub_beacon), NULL, 0);

	if (err && err != -EALREADY) {
		LOG_ERR("Hub beacon start failed: %d", err);
	}
}

static void start_scan(void)
{
	start_beacon();

	/* Active scan with the ESP32 hub's timing (interval 50 ms, window 30 ms) */
	struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = 0x50,
		.window = 0x30,
	};
	int err = bt_le_scan_start(&param, scan_cb);

	if (err && err != -EALREADY) {
		LOG_ERR("Scan start failed: %d", err);
	}
}

int tag_client_start(void)
{
	if (started) {
		return 0;
	}
	started = true;
	LOG_INF("Hub beacon on, scanning for \"%s\" tags", CONFIG_HUB_TAG_NAME);
	start_scan();
	return 0;
}

bool tag_client_busy(void)
{
	return conn != NULL;
}
