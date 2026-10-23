/*
 * The hub-queue partition is a ring of 1 KB slots, four per 4 KB erase sector. A slot holds a
 * header and one item. Items are written in order with a rising sequence number. An uploaded item
 * is marked by programming its "done" word to 0, which NOR flash allows without an erase. A
 * sector is erased just before the ring enters it, and only when none of its items still waits,
 * so the waiting items always run contiguously from the tail to the head.
 *
 * At boot the headers give the oldest waiting item (tail) and the slot after the newest (head).
 * A write cut short by a reset leaves an invalid header, which is skipped.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include "store.h"
#include "tag_format.h"

LOG_MODULE_REGISTER(store, LOG_LEVEL_INF);

#define SLOT_SIZE	 1024
#define SECTOR_SIZE	 4096
#define SLOTS_PER_SECTOR (SECTOR_SIZE / SLOT_SIZE)
#define SLOT_MAGIC	 0x51425548 /* "HUBQ" */
#define WAITING		 0xffffffff

struct slot_header {
	uint32_t magic;
	uint32_t seq;
	uint32_t crc;
	uint16_t len;
	uint16_t reserved;
	uint32_t done; /* WAITING until uploaded, then 0 */
};

BUILD_ASSERT(sizeof(struct slot_header) + TAG_QUEUE_ITEM_SIZE <= SLOT_SIZE);

static const struct flash_area *fa;
static K_MUTEX_DEFINE(lock);
static uint32_t slot_count;
static uint32_t head;
static uint32_t tail;
static uint32_t next_seq = 1;
static uint32_t waiting;

static uint32_t next_slot(uint32_t slot)
{
	return (slot + 1) % slot_count;
}

static int read_header(uint32_t slot, struct slot_header *h)
{
	return flash_area_read(fa, slot * SLOT_SIZE, h, sizeof(*h));
}

static bool header_valid(const struct slot_header *h)
{
	return h->magic == SLOT_MAGIC && h->len > 0 && h->len < TAG_QUEUE_ITEM_SIZE;
}

static bool slot_blank(uint32_t slot)
{
	uint32_t buf[16];

	for (size_t off = 0; off < SLOT_SIZE; off += sizeof(buf)) {
		if (flash_area_read(fa, slot * SLOT_SIZE + off, buf, sizeof(buf)) != 0) {
			return false;
		}
		for (size_t i = 0; i < ARRAY_SIZE(buf); i++) {
			if (buf[i] != 0xffffffff) {
				return false;
			}
		}
	}
	return true;
}

static void mark_done(uint32_t slot)
{
	uint32_t zero = 0;

	if (flash_area_write(fa, slot * SLOT_SIZE + offsetof(struct slot_header, done), &zero,
			     sizeof(zero)) != 0) {
		/* It will be uploaded again after a reset: a duplicate, not a loss */
		LOG_WRN("Could not mark slot %u as uploaded", slot);
	}
}

int store_init(void)
{
	int err = flash_area_open(FIXED_PARTITION_ID(hub_queue_partition), &fa);

	if (err) {
		LOG_ERR("No hub-queue partition (%d)", err);
		return err;
	}
	if (!device_is_ready(fa->fa_dev)) {
		LOG_ERR("External flash not ready");
		return -ENODEV;
	}
	slot_count = fa->fa_size / SLOT_SIZE;

	bool any = false;
	bool any_waiting = false;
	uint32_t newest = 0;
	uint32_t oldest_waiting = 0;
	int64_t start = k_uptime_get();

	for (uint32_t slot = 0; slot < slot_count; slot++) {
		struct slot_header h;

		if (read_header(slot, &h) != 0) {
			LOG_ERR("Reading slot %u failed", slot);
			return -EIO;
		}
		if (!header_valid(&h)) {
			continue;
		}
		if (!any || h.seq > newest) {
			any = true;
			newest = h.seq;
			head = next_slot(slot);
		}
		if (h.done == WAITING) {
			waiting++;
			if (!any_waiting || h.seq < oldest_waiting) {
				any_waiting = true;
				oldest_waiting = h.seq;
				tail = slot;
			}
		}
	}
	if (any) {
		next_seq = newest + 1;
	}

	/* A write cut short by a reset can leave the rest of the head's sector dirty */
	for (uint32_t slot = head; slot % SLOTS_PER_SECTOR != 0; slot = next_slot(slot)) {
		if (!slot_blank(slot)) {
			LOG_WRN("Slot %u is not blank, starting at the next sector", slot);
			head = ROUND_UP(head + 1, SLOTS_PER_SECTOR) % slot_count;
			break;
		}
	}
	if (!any_waiting) {
		tail = head;
	}

	LOG_INF("Flash queue: %u KB, %u slots, %u items waiting (scan %lld ms)", fa->fa_size / 1024,
		slot_count, waiting, k_uptime_get() - start);
	return 0;
}

int store_put(const char *item)
{
	size_t len = strnlen(item, TAG_QUEUE_ITEM_SIZE - 1);
	int err = 0;

	if (len == 0) {
		return 0;
	}
	k_mutex_lock(&lock, K_FOREVER);

	if (head % SLOTS_PER_SECTOR == 0) {
		uint32_t sector = head / SLOTS_PER_SECTOR;

		if (waiting > 0 && tail / SLOTS_PER_SECTOR == sector) {
			k_mutex_unlock(&lock);
			return -ENOMEM;
		}
		err = flash_area_erase(fa, sector * SECTOR_SIZE, SECTOR_SIZE);
		if (err) {
			LOG_ERR("Erasing sector %u failed (%d)", sector, err);
			k_mutex_unlock(&lock);
			return -EIO;
		}
	}

	struct slot_header h = {
		.magic = SLOT_MAGIC,
		.seq = next_seq,
		.crc = crc32_ieee((const uint8_t *)item, len),
		.len = len,
		.reserved = 0xffff,
		.done = WAITING,
	};
	uint32_t slot = head;

	/* The header goes last, so a slot cut short by a reset never looks valid */
	err = flash_area_write(fa, slot * SLOT_SIZE + sizeof(h), item, len);
	if (err == 0) {
		err = flash_area_write(fa, slot * SLOT_SIZE, &h, sizeof(h));
	}

	/* Never write into a slot twice without an erase, even after a failed write */
	head = next_slot(head);
	if (err) {
		LOG_ERR("Writing slot %u failed (%d)", slot, err);
		k_mutex_unlock(&lock);
		return -EIO;
	}
	if (waiting == 0) {
		tail = slot;
	}
	waiting++;
	next_seq++;
	k_mutex_unlock(&lock);
	return 0;
}

int store_peek(char *buf, size_t len)
{
	k_mutex_lock(&lock, K_FOREVER);

	while (waiting > 0 && tail != head) {
		struct slot_header h;

		if (read_header(tail, &h) != 0) {
			k_mutex_unlock(&lock);
			return -EIO;
		}
		if (header_valid(&h) && h.done == WAITING) {
			if (h.len < len &&
			    flash_area_read(fa, tail * SLOT_SIZE + sizeof(h), buf, h.len) == 0 &&
			    crc32_ieee((const uint8_t *)buf, h.len) == h.crc) {
				buf[h.len] = '\0';
				k_mutex_unlock(&lock);
				return h.len;
			}
			LOG_WRN("Slot %u is corrupt, skipping it", tail);
			mark_done(tail);
			waiting--;
		}
		tail = next_slot(tail);
	}
	waiting = 0;
	tail = head;
	k_mutex_unlock(&lock);
	return -ENOENT;
}

void store_pop(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (waiting > 0) {
		mark_done(tail);
		waiting--;
		tail = next_slot(tail);
	}
	k_mutex_unlock(&lock);
}

int store_count(void)
{
	return waiting;
}

int store_free(void)
{
	/* Up to a sector each is lost to partly used sectors at the head and the tail */
	int free = (int)slot_count - (int)waiting - 2 * SLOTS_PER_SECTOR;

	return MAX(free, 0);
}
