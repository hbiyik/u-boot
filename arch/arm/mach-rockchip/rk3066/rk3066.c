// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2016 Rockchip Electronics Co., Ltd
 */

#include <common.h>
#include <adc.h>
#include <g_dnl.h>
#include <spl.h>
#include <asm/io.h>
#include <asm/arch-rockchip/bootrom.h>
#include <asm/arch-rockchip/grf_rk3066.h>
#include <asm/arch-rockchip/f_rockusb.h>
#include <dm/device.h>
#include <dm/uclass.h>

#define GRF_BASE	0x20008000

const char * const boot_devices[BROM_LAST_BOOTSOURCE + 1] = {
	[BROM_BOOTSOURCE_EMMC] = "/mmc@1021c000",
	[BROM_BOOTSOURCE_SD] = "/mmc@10214000",
};

void board_debug_uart_init(void)
{
	struct rk3066_grf * const grf = (void *)GRF_BASE;

	/* Enable early UART on the RK3066 */
	rk_clrsetreg(&grf->gpio1b_iomux,
		     GPIO1B1_MASK | GPIO1B0_MASK,
		     GPIO1B1_UART2_SOUT << GPIO1B1_SHIFT |
		     GPIO1B0_UART2_SIN << GPIO1B0_SHIFT);
}

__weak void do_spl(void)
{
	if (CONFIG_IS_ENABLED(OF_PLATDATA))
		return;

#if IS_ENABLED(CONFIG_SPL_USB_FUNCTION_ROCKUSB)
	char *dev_type;
	int dev_index;
	int ret;

	switch (spl_boot_device()) {
	case BOOT_DEVICE_MMC1:
		dev_type = "mmc";
		dev_index = 0;
		break;
	default:
		return;
}

	rockusb_dev_init(dev_type, dev_index);

	ret = usb_gadget_initialize(0);
	if (ret) {
		printf("USB init failed: %d\n", ret);
		return;
	}

	g_dnl_clear_detach();
	ret = g_dnl_register("usb_dnl_rockusb");
	if (ret)
		return;

	if (!g_dnl_board_usb_cable_connected()) {
		printf("\rUSB cable not detected\n");
		goto exit;
	}

	while (1) {
		if (g_dnl_detach())
			break;
		if (ctrlc())
			break;
		usb_gadget_handle_interrupts(0);
	}

exit:
	g_dnl_unregister();
	g_dnl_clear_detach();
	usb_gadget_release(0);
#endif
	return;
}

#define KEY_DOWN_MIN_VAL	0
#define KEY_DOWN_MAX_VAL	30

__weak int rockchip_dnl_key_pressed(void)
{
#if IS_ENABLED(CONFIG_SPL_SARADC_ROCKCHIP)
unsigned int val;
	int ret;

	ret = adc_channel_single_shot("saradc@2006c000", 1, &val);

	if (ret) {
		printf("Read adc key val failed\n");
		return false;
	}

	if (val >= KEY_DOWN_MIN_VAL && val <= KEY_DOWN_MAX_VAL)
		return true;
#endif
	return false;
}


void spl_board_init(void)
{
	__maybe_unused struct rk3066_grf * const grf = (void *)GRF_BASE;

	if (!IS_ENABLED(CONFIG_SPL_BUILD))
		return;

	if (IS_ENABLED(CONFIG_NAND_BOOT))
		rk_clrreg(&grf->soc_con0, EMMC_FLASH_SEL_MASK);

	if (IS_ENABLED(CONFIG_SPL_DM_MMC)) {
		rk_clrsetreg(&grf->gpio3b_iomux,
			     GPIO3B0_MASK | GPIO3B1_MASK | GPIO3B2_MASK |
			     GPIO3B3_MASK | GPIO3B4_MASK | GPIO3B5_MASK |
			     GPIO3B6_MASK,
			     GPIO3B0_SDMMC0_CLKOUT << GPIO3B0_SHIFT |
			     GPIO3B1_SDMMC0_CMD    << GPIO3B1_SHIFT |
			     GPIO3B2_SDMMC0_DATA0  << GPIO3B2_SHIFT |
			     GPIO3B3_SDMMC0_DATA1  << GPIO3B3_SHIFT |
			     GPIO3B4_SDMMC0_DATA2  << GPIO3B4_SHIFT |
			     GPIO3B5_SDMMC0_DATA3  << GPIO3B5_SHIFT |
			     GPIO3B6_SDMMC0_DECTN  << GPIO3B6_SHIFT);
	}


	if (rockchip_dnl_key_pressed()) {
		printf("Recovery button was pressed!\n");
		do_spl();
	}
}
