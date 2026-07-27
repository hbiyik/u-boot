// SPDX-License-Identifier: GPL-2.0+
/*
 * dfu_rkmtd.c -- DFU for Rockchip rkmtd virtual block device.
 *
 * This backend only reads from / writes to an already existing rkmtd
 * block device. It intentionally does NOT bind or attach the rkmtd
 * device to its backing MTD device - that must be done beforehand
 * with other tools/commands, e.g.:
 *
 *   rkmtd bind <label>
 *
 * Each DFU alt setting for "rkmtd" accepts a single sector number
 * (a multiple of 512 bytes) within the rkmtd block device. The size
 * is always the maximum available, i.e. from that sector up to the
 * end of the usable rkmtd area (BUF_SIZE, see include/rkmtd.h - the
 * real backing-store limit, not the padded LBA count the blk device
 * reports, which also includes synthetic MBR/GPT metadata blocks).
 * This avoids configuring an alt setting that silently no-ops past
 * the real usable region.
 *
 * Multiple alt settings can still target different sectors of the
 * same rkmtd device by separating them with ';' in dfu_alt_info, and
 * multiple interfaces (e.g. rkmtd plus mmc) by separating them with
 * '&' - see the dfu_alt_info format documented in drivers/dfu/dfu.c.
 * For example, when the interface/device are not already selected on
 * the "dfu"/"ums"/"rockusb" command line, dfu_alt_info would read:
 *
 *   rkmtd label1=idb 0;idbloader.img 64
 *
 * or, once "rkmtd label1" is already selected on the command line
 * (dfu 0 rkmtd label1), just the alt list itself is enough:
 *
 *   idb 0;idbloader.img 64
 */

#include <blk.h>
#include <dfu.h>
#include <div64.h>
#include <dm.h>
#include <rkmtd.h>
#include <dm/uclass-internal.h>
#include <linux/err.h>

static int rkmtd_block_op(enum dfu_op op, struct dfu_entity *dfu,
			   u64 offset, void *buf, long *len)
{
	struct udevice *blk;
	struct blk_desc *desc;
	u64 off, lim, dev_bytes;
	lbaint_t blk_start, blk_count, n;
	int ret;

	ret = blk_get_from_parent(dfu->data.rkmtd.dev, &blk);
	if (ret) {
		pr_err("rkmtd device has no blk device - bind/attach it first\n");
		return ret;
	}

	desc = dev_get_uclass_plat(blk);
	dev_bytes = (u64)desc->lba * desc->blksz;

	off = dfu->data.rkmtd.start + offset;
	lim = dfu->data.rkmtd.start + dfu->data.rkmtd.size;
	if (lim > dev_bytes)
		lim = dev_bytes;

	if (off >= lim) {
		printf("Limit reached 0x%llx\n", lim);
		*len = 0;
		return op == DFU_OP_READ ? 0 : -EIO;
	}

	if (off + *len > lim)
		*len = lim - off;

	*len = ALIGN(*len, desc->blksz);

	blk_start = (lbaint_t)lldiv(off, desc->blksz);
	blk_count = *len / desc->blksz;

	if ((u64)(blk_start + blk_count) * desc->blksz > lim) {
		puts("Request would exceed designated rkmtd area!\n");
		return -EINVAL;
	}

	debug("%s: %s dev: %s start: %lu cnt: %lu buf: %p\n", __func__,
	      op == DFU_OP_READ ? "RKMTD READ" : "RKMTD WRITE",
	      dfu->data.rkmtd.dev->name, (unsigned long)blk_start,
	      (unsigned long)blk_count, buf);

	switch (op) {
	case DFU_OP_READ:
		n = blk_dread(desc, blk_start, blk_count, buf);
		break;
	case DFU_OP_WRITE:
		n = blk_dwrite(desc, blk_start, blk_count, buf);
		break;
	default:
		pr_err("Operation not supported\n");
		return -EINVAL;
	}

	if (n != blk_count) {
		pr_err("RKMTD operation failed\n");
		return -EIO;
	}

	return 0;
}

static int dfu_get_medium_size_rkmtd(struct dfu_entity *dfu, u64 *size)
{
	*size = dfu->data.rkmtd.size;

	return 0;
}

static int dfu_read_medium_rkmtd(struct dfu_entity *dfu, u64 offset,
				  void *buf, long *len)
{
	int ret = -1;

	switch (dfu->layout) {
	case DFU_RAW_ADDR:
		ret = rkmtd_block_op(DFU_OP_READ, dfu, offset, buf, len);
		break;
	default:
		printf("%s: Layout (%s) not (yet) supported!\n", __func__,
		       dfu_get_layout(dfu->layout));
	}

	return ret;
}

static int dfu_write_medium_rkmtd(struct dfu_entity *dfu, u64 offset,
				   void *buf, long *len)
{
	int ret = -1;

	switch (dfu->layout) {
	case DFU_RAW_ADDR:
		ret = rkmtd_block_op(DFU_OP_WRITE, dfu, offset, buf, len);
		break;
	default:
		printf("%s: Layout (%s) not (yet) supported!\n", __func__,
		       dfu_get_layout(dfu->layout));
	}

	return ret;
}

static int dfu_flush_medium_rkmtd(struct dfu_entity *dfu)
{
	return 0;
}

int dfu_fill_entity_rkmtd(struct dfu_entity *dfu, char *devstr, char **argv,
			  int argc)
{
	struct udevice *dev;
	struct udevice *blk;
	struct blk_desc *desc;
	u64 dev_bytes, usable_bytes, sector;
	char *s;
	int ret;

	dev = rkmtd_find_by_label(devstr);
	if (!dev) {
		int devnum;
		char *ep;

		devnum = hextoul(devstr, &ep);
		if (*ep ||
		    uclass_find_device_by_seq(UCLASS_RKMTD, devnum, &dev)) {
			pr_err("No such rkmtd device '%s'\n", devstr);
			return -ENODEV;
		}
	}

	ret = blk_get_from_parent(dev, &blk);
	if (ret) {
		pr_err("rkmtd device '%s' has no blk device - bind/attach it first\n",
		       devstr);
		return ret;
	}
	desc = dev_get_uclass_plat(blk);
	dev_bytes = (u64)desc->lba * desc->blksz;

	usable_bytes = BUF_SIZE;
	if (usable_bytes > dev_bytes)
		usable_bytes = dev_bytes;

	if (argc != 1) {
		pr_err("rkmtd requires a single <sector> argument\n");
		return -EINVAL;
	}

	dfu->layout = DFU_RAW_ADDR;
	dfu->data.rkmtd.dev = dev;

	sector = simple_strtoull(argv[0], &s, 0);
	if (*s)
		return -EINVAL;

	dfu->data.rkmtd.start = sector << 9;

	if (dfu->data.rkmtd.start >= usable_bytes) {
		pr_err("rkmtd sector %llu (offset 0x%llx) exceeds usable rkmtd area 0x%llx\n",
		       sector, dfu->data.rkmtd.start, usable_bytes);
		return -EINVAL;
	}

	dfu->data.rkmtd.size = usable_bytes - dfu->data.rkmtd.start;

	dfu->dev_type = DFU_DEV_RKMTD;
	dfu->max_buf_size = 0;
	dfu->get_medium_size = dfu_get_medium_size_rkmtd;
	dfu->read_medium = dfu_read_medium_rkmtd;
	dfu->write_medium = dfu_write_medium_rkmtd;
	dfu->flush_medium = dfu_flush_medium_rkmtd;
	dfu->inited = 0;

	return 0;
}
