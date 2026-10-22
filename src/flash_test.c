/*
 * W25Q64JV external flash check over SPIM2 (CS P1.01, CLK P1.04, DO P1.02, DIO P1.06).
 * The spi-nor driver reads size and erase types from the chip's SFDP tables, so a driver that
 * comes up ready already proves the SPI wiring.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/printk.h>

#include "board_tests.h"

#define SECTOR_SIZE 4096
#define PATTERN_LEN 256

static const struct device *const flash = DEVICE_DT_GET(DT_ALIAS(spi_flash0));

static uint8_t buf[PATTERN_LEN];

static size_t flash_size(void)
{
	struct flash_pages_info info;
	size_t pages = flash_get_page_count(flash);

	if (pages == 0 || flash_get_page_info_by_idx(flash, 0, &info) != 0) {
		return 0;
	}
	return pages * info.size;
}

static void print_hex(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		printk("%02x ", data[i]);
	}
	printk("\n");
}

void flash_test_info(void)
{
	printk("\n--- External flash ---\n");
	if (!device_is_ready(flash)) {
		printk("Flash not ready: the spi-nor driver could not read a valid JEDEC ID / SFDP\n");
		return;
	}

	uint8_t id[3];

	if (flash_read_jedec_id(flash, id) == 0) {
		/* W25Q64JV is ef 40 17 (Winbond, 64 Mbit) */
		printk("JEDEC ID %02x %02x %02x\n", id[0], id[1], id[2]);
	}
	printk("Size %u bytes\n", flash_size());

	if (flash_read(flash, 0, buf, 16) == 0) {
		printk("First 16 bytes: ");
		print_hex(buf, 16);
	}
}

void flash_test_write(void)
{
	size_t size = flash_size();

	if (!device_is_ready(flash) || size < SECTOR_SIZE) {
		printk("Flash not ready, skipping the write test\n");
		return;
	}

	off_t offset = size - SECTOR_SIZE;
	uint32_t seed = k_cycle_get_32();

	printk("\n--- Flash write test, sector 0x%06lx ---\n", (long)offset);

	int64_t t = k_uptime_get();
	int err = flash_erase(flash, offset, SECTOR_SIZE);

	printk("Erase: %d (%lld ms)\n", err, k_uptime_get() - t);
	if (err) {
		return;
	}

	flash_read(flash, offset, buf, PATTERN_LEN);
	for (size_t i = 0; i < PATTERN_LEN; i++) {
		if (buf[i] != 0xFF) {
			printk("Not blank after erase at +%u: %02x\n", i, buf[i]);
			return;
		}
	}

	for (size_t i = 0; i < PATTERN_LEN; i++) {
		buf[i] = (uint8_t)(seed >> ((i % 4) * 8)) ^ (uint8_t)i;
	}
	t = k_uptime_get();
	err = flash_write(flash, offset, buf, PATTERN_LEN);
	printk("Write %u bytes: %d (%lld ms)\n", PATTERN_LEN, err, k_uptime_get() - t);
	if (err) {
		return;
	}

	static uint8_t check[PATTERN_LEN];

	flash_read(flash, offset, check, PATTERN_LEN);
	if (memcmp(buf, check, PATTERN_LEN) == 0) {
		printk("Read back matches: flash write OK\n");
	} else {
		printk("Read back MISMATCH. Wrote: ");
		print_hex(buf, 16);
		printk("Read:  ");
		print_hex(check, 16);
	}
}
