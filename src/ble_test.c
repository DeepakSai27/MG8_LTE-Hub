/*
 * BLE radio check: advertises (non-connectable) as CONFIG_BT_DEVICE_NAME so a phone scanner can
 * see the MG8, and passively scans so we can see that the receiver works. Every 30 s it prints how
 * many devices were heard and flags any Exactcomms C10 tag.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>

#include "board_tests.h"

#define MAX_DEVICES	64
#define REPORT_INTERVAL K_SECONDS(30)
#define C10_NAME	"exactcomms_c10"

struct seen_device {
	bt_addr_le_t addr;
	int8_t rssi;
	char name[24];
};

static struct seen_device devices[MAX_DEVICES];
static size_t device_count;
static uint32_t adv_reports;
static bool table_full;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static bool find_name(struct bt_data *data, void *user_data)
{
	char *name = user_data;

	if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
		size_t len = MIN(data->data_len, sizeof(devices[0].name) - 1);

		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	}
	return true;
}

static struct seen_device *lookup(const bt_addr_le_t *addr)
{
	for (size_t i = 0; i < device_count; i++) {
		if (bt_addr_le_eq(&devices[i].addr, addr)) {
			return &devices[i];
		}
	}
	if (device_count == MAX_DEVICES) {
		table_full = true;
		return NULL;
	}
	struct seen_device *dev = &devices[device_count++];

	bt_addr_le_copy(&dev->addr, addr);
	dev->rssi = INT8_MIN;
	dev->name[0] = '\0';
	return dev;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	struct seen_device *dev = lookup(addr);

	adv_reports++;
	if (dev == NULL) {
		return;
	}
	dev->rssi = MAX(dev->rssi, rssi);
	if (dev->name[0] == '\0') {
		bt_data_parse(buf, find_name, dev->name);
		if (strstr(dev->name, C10_NAME) != NULL) {
			char addr_str[BT_ADDR_LE_STR_LEN];

			bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
			printk("[ble] C10 tag found: %s %s RSSI %d\n", dev->name, addr_str, rssi);
		}
	}
}

void ble_test_report(void)
{
	const struct seen_device *best = NULL;
	size_t named = 0;

	for (size_t i = 0; i < device_count; i++) {
		if (best == NULL || devices[i].rssi > best->rssi) {
			best = &devices[i];
		}
		named += devices[i].name[0] != '\0';
	}
	printk("[ble] %u advertising reports from %u%s devices (%u named)", adv_reports,
	       device_count, table_full ? "+" : "", named);
	if (best != NULL) {
		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&best->addr, addr_str, sizeof(addr_str));
		printk(", strongest %s %s RSSI %d", addr_str, best->name, best->rssi);
	}
	printk("\n");
}

static void report_handler(struct k_work *work)
{
	ble_test_report();
	k_work_reschedule(k_work_delayable_from_work(work), REPORT_INTERVAL);
}

static K_WORK_DELAYABLE_DEFINE(report_work, report_handler);

void ble_test_start(void)
{
	printk("\n--- BLE ---\n");

	int err = bt_enable(NULL);

	if (err) {
		printk("bt_enable failed: %d\n", err);
		return;
	}

	bt_addr_le_t addr;
	size_t count = 1;

	bt_id_get(&addr, &count);
	if (count > 0) {
		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));
		printk("BLE address %s\n", addr_str);
	}

	err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad), NULL, 0);
	printk("Advertising as \"%s\": %d\n", CONFIG_BT_DEVICE_NAME, err);

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_cb);
	printk("Passive scan: %d (summary every 30 s, or type !ble)\n", err);

	k_work_reschedule(&report_work, REPORT_INTERVAL);
}
