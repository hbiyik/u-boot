/*
 * (C) Copyright 2024 boogie
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#ifndef __RESC_FAT_H_
#define __RESC_FAT_H_

#include <common.h>

int rockchip_get_fat_part(struct blk_desc **desc, disk_partition_t *part);
int rockchip_read_fat_file(void *buf, const char *name, struct blk_desc **desc);
int rockchip_write_fat_file(void *buf, const char *name, loff_t maxsize, struct blk_desc **desc);

#endif
