// SPDX-License-Identifier: GPL-2.0+

#include <div64.h>
#include <log.h>
#include <malloc.h>
#include <dfu.h>
#include <vsprintf.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/mtd/ubi.h>
#include <mtd/ubi-user.h>
#include <ubi_uboot.h>

static int ubi_dfu_open(struct dfu_entity *dfu, int mode)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;

	if (ud->desc)
		return 0;

	ud->desc = ubi_open_volume(ud->ubi_num, ud->vol_id, mode);
	if (IS_ERR(ud->desc)) {
		int err = PTR_ERR(ud->desc);

		ud->desc = NULL;
		pr_err("error: ubi: cannot open volume %d on ubi%d (%d)\n",
		       ud->vol_id, ud->ubi_num, err);
		return err;
	}

	return 0;
}

static void ubi_dfu_close(struct dfu_entity *dfu)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;

	if (ud->desc) {
		ubi_close_volume(ud->desc);
		ud->desc = NULL;
	}
}

static void ubi_dfu_off_to_leb(struct ubi_internal_data *ud, u64 offset,
			       int *lnum, u32 *leb_off)
{
	u64 n = offset;

	*leb_off = do_div(n, ud->usable_leb_size);
	*lnum = (int)n;
}

static int dfu_get_medium_size_ubi(struct dfu_entity *dfu, u64 *size)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;

	*size = (u64)ud->leb_count * ud->usable_leb_size;

	return 0;
}

static int dfu_read_medium_ubi(struct dfu_entity *dfu, u64 offset, void *buf,
			       long *len)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;
	long left = *len;
	long done = 0;
	int lnum, ret;
	u32 leb_off;

	if (dfu->layout != DFU_RAW_ADDR) {
		pr_err("error: ubi: layout (%s) is not (yet) supported!\n",
		       dfu_get_layout(dfu->layout));
		return -1;
	}

	ret = ubi_dfu_open(dfu, UBI_READONLY);
	if (ret)
		return ret;

	ubi_dfu_off_to_leb(ud, offset, &lnum, &leb_off);

	while (left > 0) {
		int chunk;

		if (lnum >= ud->leb_count)
			break;

		chunk = ud->usable_leb_size - leb_off;
		if (chunk > left)
			chunk = left;

		ret = ubi_leb_read(ud->desc, lnum, (char *)buf + done, leb_off,
				   chunk, 0);
		if (ret) {
			pr_err("error: ubi: read error on volume '%s', LEB %d (%d)\n",
			       ud->vol_name, lnum, ret);
			return ret;
		}

		done += chunk;
		left -= chunk;
		lnum++;
		leb_off = 0;
	}

	*len = done;

	return 0;
}

static int dfu_write_medium_ubi_dynamic(struct dfu_entity *dfu, u64 offset,
					 void *buf, long *len)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;
	long left = *len;
	long done = 0;
	int lnum, ret;
	u32 leb_off;

	ret = ubi_dfu_open(dfu, UBI_READWRITE);
	if (ret)
		return ret;

	if (offset == 0) {
		for (lnum = 0; lnum < ud->leb_count; lnum++) {
			ret = ubi_leb_unmap(ud->desc, lnum);
			if (ret) {
				pr_err("error: ubi: failed to erase LEB %d of volume '%s' (%d)\n",
				       lnum, ud->vol_name, ret);
				return ret;
			}
		}
	}

	ubi_dfu_off_to_leb(ud, offset, &lnum, &leb_off);

	while (left > 0) {
		int chunk;

		if (lnum >= ud->leb_count) {
			pr_err("error: ubi: volume '%s' (%d LEBs, %u bytes/LEB) is too small for the image\n",
			       ud->vol_name, ud->leb_count,
			       ud->usable_leb_size);
			return -ENOSPC;
		}

		chunk = ud->usable_leb_size - leb_off;
		if (chunk > left)
			chunk = left;

		if (!IS_ALIGNED(chunk, ud->min_io_size)) {
			unsigned int padded = round_up(chunk, ud->min_io_size);
			u8 *pad_buf = malloc(ud->min_io_size);

			if (!pad_buf) {
				pr_err("error: ubi: out of memory allocating pad buffer\n");
				return -ENOMEM;
			}

			memcpy(pad_buf, (u8 *)buf + done, chunk);
			memset(pad_buf + chunk, 0xff, padded - chunk);

			ret = ubi_leb_write(ud->desc, lnum, pad_buf, leb_off,
					    padded);

			free(pad_buf);
		} else {
			ret = ubi_leb_write(ud->desc, lnum, (u8 *)buf + done,
					    leb_off, chunk);
		}

		if (ret) {
			pr_err("error: ubi: write error on volume '%s', LEB %d (%d)\n",
			       ud->vol_name, lnum, ret);
			return ret;
		}

		done += chunk;
		left -= chunk;
		lnum++;
		leb_off = 0;
	}

	*len = done;

	return 0;
}

static int dfu_write_medium_ubi_static(struct dfu_entity *dfu, u64 offset,
					void *buf, long *len)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;

	if (offset == 0 && ud->wr_buf) {
		free(ud->wr_buf);
		ud->wr_buf = NULL;
		ud->wr_len = 0;
		ud->wr_cap = 0;
	}

	if (ud->wr_len + *len > ud->wr_cap) {
		size_t new_cap = ud->wr_cap ? ud->wr_cap * 2 : (1 << 20);
		u8 *new_buf;

		while (new_cap < ud->wr_len + (size_t)*len)
			new_cap *= 2;

		new_buf = realloc(ud->wr_buf, new_cap);
		if (!new_buf) {
			pr_err("error: ubi: out of memory staging '%s' (%zu bytes)\n",
			       ud->vol_name, new_cap);
			return -ENOMEM;
		}
		ud->wr_buf = new_buf;
		ud->wr_cap = new_cap;
	}

	memcpy(ud->wr_buf + ud->wr_len, buf, *len);
	ud->wr_len += *len;

	return 0;
}

static int dfu_write_medium_ubi(struct dfu_entity *dfu, u64 offset, void *buf,
				long *len)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;

	if (dfu->layout != DFU_RAW_ADDR) {
		pr_err("error: ubi: layout (%s) is not (yet) supported!\n",
		       dfu_get_layout(dfu->layout));
		return -1;
	}

	if (ud->vol_type == UBI_STATIC_VOLUME)
		return dfu_write_medium_ubi_static(dfu, offset, buf, len);

	return dfu_write_medium_ubi_dynamic(dfu, offset, buf, len);
}

static int dfu_flush_medium_ubi(struct dfu_entity *dfu)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;
	int ret;

	if (ud->vol_type != UBI_STATIC_VOLUME || !ud->wr_buf)
		return 0;

	ret = ubi_volume_write(ud->vol_name, ud->wr_buf, 0, ud->wr_len);

	free(ud->wr_buf);
	ud->wr_buf = NULL;
	ud->wr_len = 0;
	ud->wr_cap = 0;

	if (ret) {
		pr_err("error: ubi: failed to write volume '%s' (%d)\n",
		       ud->vol_name, ret);
		return ret;
	}

	return 0;
}

static void dfu_free_entity_ubi(struct dfu_entity *dfu)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;

	free(ud->wr_buf);
	ud->wr_buf = NULL;
	ubi_dfu_close(dfu);
}

int dfu_fill_entity_ubi(struct dfu_entity *dfu, char *devstr, char **argv,
			int argc)
{
	struct ubi_internal_data *ud = &dfu->data.ubi;
	struct ubi_volume_desc *desc;
	struct ubi_device_info di;
	struct ubi_volume_info vi;
	char *endp;
	int ret, name_len;

	memset(ud, 0, sizeof(*ud));

	ud->ubi_num = dectoul(devstr, &endp);
	if (*endp) {
		pr_err("error: ubi: invalid ubi device number '%s'\n", devstr);
		return -EINVAL;
	}

	ret = ubi_get_device_info(ud->ubi_num, &di);
	if (ret) {
		pr_err("error: ubi: ubi%d is not attached - run 'ubi part <partition>' before dfu\n",
		       ud->ubi_num);
		return ret;
	}

	if (argc != 2) {
		pr_err("error: ubi: bad arguments, expected \"vol <name>\" or \"volid <id>\"\n");
		return -EINVAL;
	}

	if (!strcmp(argv[0], "vol")) {
		desc = ubi_open_volume_nm(ud->ubi_num, argv[1], UBI_READONLY);
	} else if (!strcmp(argv[0], "volid")) {
		int vol_id = dectoul(argv[1], &endp);

		if (*endp) {
			pr_err("error :ubi: invalid volume id '%s'\n", argv[1]);
			return -EINVAL;
		}
		desc = ubi_open_volume(ud->ubi_num, vol_id, UBI_READONLY);
	} else {
		pr_err("error: ubi: unsupported arg '%s' - use \"vol\" or \"volid\"\n",
		       argv[0]);
		return -EINVAL;
	}

	if (IS_ERR(desc)) {
		ret = PTR_ERR(desc);
		pr_err("error: ubi: cannot find volume '%s' on ubi%d (%d)\n",
		       argv[1], ud->ubi_num, ret);
		return ret;
	}

	ubi_get_volume_info(desc, &vi);

	ud->vol_id = vi.vol_id;
	ud->vol_type = vi.vol_type;
	ud->usable_leb_size = vi.usable_leb_size;
	ud->min_io_size = di.min_io_size;
	ud->leb_count = vi.size;

	name_len = vi.name_len;
	if (name_len >= sizeof(ud->vol_name))
		name_len = sizeof(ud->vol_name) - 1;
	memcpy(ud->vol_name, vi.name, name_len);
	ud->vol_name[name_len] = '\0';

	/* done probing - reopen lazily with the mode a transfer needs */
	ubi_close_volume(desc);

	if (!ud->usable_leb_size || !ud->leb_count) {
		pr_err("error: ubi: volume '%s' has zero capacity\n",
		       ud->vol_name);
		return -EINVAL;
	}

	dfu->dev_type = DFU_DEV_UBI;
	dfu->layout = DFU_RAW_ADDR;
	dfu->max_buf_size = ud->min_io_size;
	dfu->get_medium_size = dfu_get_medium_size_ubi;
	dfu->read_medium = dfu_read_medium_ubi;
	dfu->write_medium = dfu_write_medium_ubi;
	dfu->flush_medium = dfu_flush_medium_ubi;
	dfu->free_entity = dfu_free_entity_ubi;

	dfu->inited = 0;

	return 0;
}
