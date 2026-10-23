/*
 * Upload queue items kept in the external W25Q64 flash, so they survive resets, power loss and
 * long LTE outages. Items come out in the order they went in.
 */

#pragma once

#include <stddef.h>

/* Finds the waiting items left from before the reset. 0, or a negative error without flash. */
int store_init(void);

/* Appends one NUL-terminated item. 0, -ENOMEM when full, or -EIO. */
int store_put(const char *item);

/* Copies the oldest waiting item into buf (NUL-terminated). Its length, or -ENOENT if none. */
int store_peek(char *buf, size_t len);

/* Marks the item returned by the last store_peek() as uploaded */
void store_pop(void);

/* Items waiting, and items that can still be added */
int store_count(void);
int store_free(void);
