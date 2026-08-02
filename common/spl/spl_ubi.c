// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (C) 2016
 * Ladislav Michl <ladis@linux-mips.org>
 */

#include <config.h>
#include <image.h>
#include <nand.h>
#include <onenand_uboot.h>
#include <ubispl.h>
#include <spl.h>
#include <mtd.h>

#if IS_ENABLED(CONFIG_SPL_UBI_MTD)
__weak int nand_spl_read_block(int block, int offset, int len, void *dst)
{
    static struct mtd_info *mtd = NULL;
    static loff_t *block_map = NULL;
    static int max_blocks = 0;

    size_t retlen;

    if (!mtd) {
        struct mtd_info *nand = NULL;
        for (int i = 0; i < CONFIG_SYS_MAX_NAND_DEVICE; i++) {
            nand = get_nand_dev_by_index(i);
            if (IS_ERR_OR_NULL(nand))
                continue;

            if (list_empty(&nand->partitions))
                add_mtd_partitions_of(nand);
        }

        mtd = get_mtd_device_nm(CONFIG_SPL_UBI_MTD_NAME);
        if (IS_ERR_OR_NULL(mtd)) {
            mtd = NULL;
            return -ENODEV;
        }

        loff_t addr = 0;
        int total_ph_blocks = mtd->size / mtd->erasesize;
        int logical_index = 0;

        block_map = malloc(total_ph_blocks * sizeof(loff_t));
        if (!block_map) {
            printf("Error: Failed to allocate memory for block map\n");
            return -ENOMEM;
        }

        printf("Scanning MTD device and building block map...\n");
        for (int i = 0; i < total_ph_blocks; i++) {
            if (!mtd_block_isbad(mtd, addr)) {
                block_map[logical_index++] = addr;
            }
            addr += mtd->erasesize;
        }
        max_blocks = logical_index;
        printf("Scan complete: Found %d good blocks out of %d physical blocks.\n", max_blocks, total_ph_blocks);
    }

    if (block < 0 || block >= max_blocks) {
        return -EINVAL;
    }

    loff_t addr = block_map[block];
    
    if (mtd_block_isbad(mtd, addr))
        return -EIO;

    return mtd_read(mtd, addr + offset, len, &retlen, dst);
}
#endif

#if IS_ENABLED(CONFIG_SPL_OS_BOOT)
int spl_ubi_load_image_os(struct spl_image_info *spl_image,
			  struct spl_boot_device *bootdev,
			  struct ubispl_info *info)
{
	struct legacy_img_hdr *header;
	struct ubispl_load volumes[2];
	int err;

	volumes[0].vol_id = CONFIG_SPL_UBI_LOAD_KERNEL_ID;
	volumes[0].load_addr = (void *)CONFIG_SYS_LOAD_ADDR;
#if IS_ENABLED(CONFIG_SPL_OS_BOOT_ARGS)
	volumes[1].vol_id = CONFIG_SPL_UBI_LOAD_ARGS_ID;
	volumes[1].load_addr = (void *)CONFIG_SPL_PAYLOAD_ARGS_ADDR;

	err = ubispl_load_volumes(info, volumes, 2);
#else
	err = ubispl_load_volumes(info, volumes, 1);
#endif
	if (err)
		return err;

	header = (struct legacy_img_hdr *)volumes[0].load_addr;
	spl_parse_image_header(spl_image, bootdev, header);
	puts("Linux loaded.\n");

	return 0;
}
#endif

int spl_ubi_load_image(struct spl_image_info *spl_image,
		       struct spl_boot_device *bootdev)
{
	struct legacy_img_hdr *header;
	struct ubispl_info info;
	struct ubispl_load volumes[2];
	int ret = 1;

	switch (bootdev->boot_device) {
#ifdef CONFIG_SPL_NAND_SUPPORT
	case BOOT_DEVICE_NAND:
		nand_init();
		info.read = nand_spl_read_block;
		info.peb_size = CONFIG_SYS_NAND_BLOCK_SIZE;
		break;
#endif
#ifdef CONFIG_SPL_ONENAND_SUPPORT
	case BOOT_DEVICE_ONENAND:
		info.read = onenand_spl_read_block;
		info.peb_size = CFG_SYS_ONENAND_BLOCK_SIZE;
		break;
#endif
	default:
		goto out;
	}
	info.ubi = (struct ubi_scan_info *)CONFIG_SPL_UBI_INFO_ADDR;
	info.fastmap = IS_ENABLED(CONFIG_MTD_UBI_FASTMAP);

	info.peb_offset = CONFIG_SPL_UBI_PEB_OFFSET;
	info.vid_offset = CONFIG_SPL_UBI_VID_OFFSET;
	info.leb_start = CONFIG_SPL_UBI_LEB_START;
	info.peb_count = CONFIG_SPL_UBI_MAX_PEBS - info.peb_offset;

#if CONFIG_IS_ENABLED(OS_BOOT)
	if (!spl_start_uboot()) {
		ret = spl_ubi_load_image_os(spl_image, bootdev, &info);
		if (!ret)
			return 0;

		printf("%s: Failed in falcon boot: %d", __func__, ret);
		if (IS_ENABLED(CONFIG_SPL_OS_BOOT_SECURE))
			return ret;
		printf("Fallback to U-Boot\n");
	}
#endif

	header = spl_get_load_buffer(-sizeof(*header), sizeof(header));
#ifdef CONFIG_SPL_UBI_LOAD_BY_VOLNAME
	volumes[0].vol_id = -1;
	strncpy(volumes[0].name,
		CONFIG_SPL_UBI_LOAD_MONITOR_VOLNAME,
		UBI_VOL_NAME_MAX + 1);
#else
	volumes[0].vol_id = CONFIG_SPL_UBI_LOAD_MONITOR_ID;
#endif
	volumes[0].load_addr = (void *)header;

	ret = ubispl_load_volumes(&info, volumes, 1);
	if (!ret)
		spl_parse_image_header(spl_image, bootdev, header);
out:
#ifdef CONFIG_SPL_NAND_SUPPORT
	if (bootdev->boot_device == BOOT_DEVICE_NAND)
		nand_deselect();
#endif
	return ret;
}
/* Use priorty 0 so that Ubi will override NAND and ONENAND methods */
SPL_LOAD_IMAGE_METHOD("NAND", 0, BOOT_DEVICE_NAND, spl_ubi_load_image);
SPL_LOAD_IMAGE_METHOD("OneNAND", 0, BOOT_DEVICE_ONENAND, spl_ubi_load_image);
