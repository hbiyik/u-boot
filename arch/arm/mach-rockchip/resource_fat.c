/*
 * (C) Copyright 2017 Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */
#include <common.h>
#include <asm/arch/resource_fat.h>
#include <boot_rkimg.h>
#include <fat.h>

int rockchip_get_fat_part(struct blk_desc **desc, disk_partition_t *part) {

    if (!desc)
        return -ENODEV;

    if (!*desc)
        *desc = rockchip_get_bootdev();

    if (!*desc)
        return -ENODEV;

    if (part_get_info_by_name(*desc, CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME, part) < 0) {
        printf("%s: Failed to get info for %s FAT partition\n", __func__,
                CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME);
        return -ENODEV;
    }

    if (fat_set_blk_dev(*desc, part) != 0) {
        printf("%s: Failed to set resource block device %s FAT partition\n", __func__,
                CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME);
        return -ENODEV;
    }
    return 0;
}

int rockchip_read_fat_file(void *buf, const char *name, struct blk_desc **desc) {
    disk_partition_t part;
    loff_t actread, len;

    if (rockchip_get_fat_part(desc, &part))
        return -ENODEV;

    if (fat_exists(name) != 1) {
        printf("%s: File %s does not exist in %s FAT partition\n",  __func__,
                name, CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME);
        return -ENOENT;
    }

    if (fat_size(name, &len) < 0) {
        printf("%s: Failed to get file size for %s in %s FAT partition\n",  __func__,
                name, CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME);
        return -ENOENT;
    }

    if (!buf) {
        printf("%s: Failed to allocate memory for file %s\n",  __func__, name);
        return -ENOMEM;
    }

    actread = file_fat_read(name, buf, len);
    if (actread != len) {
        printf("%s: Failed to read file %s: read %lld, expected %lld in %s FAT partition\n",  __func__,
                name, actread, len, CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME);
        return -EIO;
    }

    printf("%s: File %s is read from %s FAT partiton\n",  __func__,
            name, CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME);

    return len;
}


int rockchip_write_fat_file(void *buf, const char *name, loff_t maxsize, struct blk_desc **desc) {
    disk_partition_t part;
    loff_t len;

    if (rockchip_get_fat_part(desc, &part))
        return -ENODEV;

    if (file_fat_write(name, buf, 0, maxsize, &len) != 0) {
        printf("%s: Failed to write file '%s' to device FAT partition %s\n",  __func__,
                name, CONFIG_ROCKCHIP_RESOURCE_FAT_PARTNAME);
        return -ENOENT;
    }

    return len;
}
