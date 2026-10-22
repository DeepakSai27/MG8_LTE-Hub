/* MG8 bring-up checks for the parts other than the modem: W25Q64 flash and BLE */

#pragma once

/* Prints the flash JEDEC ID, size and first bytes. Read-only. */
void flash_test_info(void);

/* Erases the last 4 KB sector, writes a pattern, reads it back and compares. */
void flash_test_write(void);

/* Starts BLE advertising as CONFIG_BT_DEVICE_NAME and a passive scan, reporting every 30 s */
void ble_test_start(void);

/* Prints what the scan has seen so far */
void ble_test_report(void);
