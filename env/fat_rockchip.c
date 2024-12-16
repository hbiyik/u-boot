/*
 * (C) Copyright 2017 Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */
#include <common.h>
#include <asm/arch/resource_fat.h>

#include <command.h>
#include <environment.h>
#include <memalign.h>

#ifdef CONFIG_SPL_BUILD
#  define LOADENV
#else
# define LOADENV
# if defined(CONFIG_CMD_SAVEENV)
#  define CMD_SAVEENV
# endif
#endif

#ifdef CMD_SAVEENV
static int rockchip_env_fat_save(void)
{
    ALLOC_CACHE_ALIGN_BUFFER(env_t, env_new, CONFIG_ENV_SIZE);
    struct blk_desc *desc = NULL;
    int err;

    err = env_export(env_new);
    if (err)
        return err;

    if(rockchip_write_fat_file((void *)env_new, CONFIG_ENV_FAT_FILE, sizeof(env_t), &desc) < 0)
        return 1;

    return 0;
}
#endif /* CMD_SAVEENV */

#ifdef LOADENV
static int rockchip_env_fat_load(void)
{
    ALLOC_CACHE_ALIGN_BUFFER(char, buf, CONFIG_ENV_SIZE);
    struct blk_desc *desc = NULL;

    if(rockchip_read_fat_file(buf, CONFIG_ENV_FAT_FILE, &desc) < 0)
        goto err_env_relocate;

    env_import(buf, 1);
    return 0;

err_env_relocate:
    set_default_env(NULL);

    return -EIO;
}
#endif /* LOADENV */


U_BOOT_ENV_LOCATION(fat) = {
    .location   = ENVL_FAT,
    ENV_NAME("FAT")
#ifdef LOADENV
    .load       = rockchip_env_fat_load,
#endif
#ifdef CMD_SAVEENV
    .save       = env_save_ptr(rockchip_env_fat_save),
#endif
};
