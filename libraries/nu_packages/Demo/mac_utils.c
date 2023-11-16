/**************************************************************************//**
*
* @copyright (C) 2019 Nuvoton Technology Corp. All rights reserved.
*
* SPDX-License-Identifier: Apache-2.0
*
* Change Logs:
* Date            Author       Notes
* 2023-11-10      Wayne        First version
*
******************************************************************************/

#include <rtthread.h>

#if defined(BSP_USING_EMAC) && defined(ARCH_ARM_ARM9)

#include <dfs_file.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#if defined(RT_HWCRYPTO_USING_RNG)
    #include <hw_rng.h>
#else
    #include <stdlib.h>
    #include <time.h>
#endif

#include "drv_emac.h"

#if !defined(DEF_IFACE_NAME)
    #define DEF_IFACE_NAME    "e0"
#endif

#if !defined(PATH_MACADDR_FILE)
    #define PATH_MACADDR_FILE "/mnt/filesystem/mac"
#endif

static int nu_mac_addr_restore(uint8_t *au8MacAddr)
{
    int fd = -1;

    if ((fd = open(PATH_MACADDR_FILE, O_RDONLY, 0)) < 0)
    {
        rt_kprintf("[%s] open %s failure\n", __func__, PATH_MACADDR_FILE);
        goto exit_nu_mac_addr_restore;
    }
    else if (read(fd, au8MacAddr, 6) != 6)
    {
        rt_kprintf("[%s] read %s failure\n", __func__, PATH_MACADDR_FILE);
        goto exit_nu_mac_addr_restore;
    }

    close(fd);

    rt_kprintf("[%s] read %s ok\n", __func__, PATH_MACADDR_FILE);

    return 0;

exit_nu_mac_addr_restore:

    if (fd >= 0)
        close(fd);

    return -1;
}

static int nu_mac_addr_store(void *buf, int buf_len)
{
    int fd;

    if ((fd = open(PATH_MACADDR_FILE, O_WRONLY | O_CREAT, 0)) < 0)
    {
        rt_kprintf("[%s] open %s failure\n", __func__, PATH_MACADDR_FILE);

        goto exit_nu_mac_addr_store;
    }
    else if (write(fd, buf, buf_len) == buf_len)
    {
        rt_kprintf("[%s] %s\n", __func__, PATH_MACADDR_FILE);
    }

    close(fd);

    return 0;

exit_nu_mac_addr_store:

    return -1;
}

int mac_restore(void)
{
    uint8_t au8MacAddr[6] = {0x82, 0x06, 0x21, 0x94, 0x53, 0x01};

    nu_mac_addr_restore(au8MacAddr);

#if defined(BSP_USING_EMAC0)
    nu_emac_assign_mac_addr(EMAC0_IDX, au8MacAddr);
#endif

#if defined(BSP_USING_EMAC1)
    au8MacAddr[6]++;
    nu_emac_assign_mac_addr(EMAC1_IDX, au8MacAddr);
#endif

    return 0;
}
INIT_APP_EXPORT(mac_restore);

static int mac_change(void)
{
    rt_device_t dev = RT_NULL;

#if defined(BSP_USING_EMAC0)
    dev = rt_device_find(DEF_IFACE_NAME);
#endif
    if (dev)
    {
        int i;
        uint32_t result = 0;
        uint8_t au8MacAddr[6] = {0x82, 0x06, 0x21, 0x94, 0x94, 0x94};

#if !defined(RT_HWCRYPTO_USING_RNG)
        srand(time(NULL));
#endif

        /* OUI */
        for (i = 3; i < 6; i++)
        {
#if defined(RT_HWCRYPTO_USING_RNG)
            au8MacAddr[i] = rt_hwcrypto_rng_update() & 0xff;
#else

            au8MacAddr[i] = rand() % 256;
#endif
        }

        rt_kprintf("[%s] Random Mac address: %02X:%02X:%02X:%02X:%02X:%02X\n", \
                   DEF_IFACE_NAME, \
                   au8MacAddr[0], \
                   au8MacAddr[1], \
                   au8MacAddr[2], \
                   au8MacAddr[3], \
                   au8MacAddr[4], \
                   au8MacAddr[5]);

        nu_mac_addr_store(au8MacAddr, sizeof(au8MacAddr));

        return rt_device_control(dev, NIOCTL_PRIV_CHANGE_MAC, au8MacAddr);
    }

    rt_kprintf("%s Can not find the iface %s\n", __func__, DEF_IFACE_NAME);

    return -1;
}
MSH_CMD_EXPORT(mac_change, change random mac address);

#endif /* defined(BSP_USING_EMAC) && defined(SOC_SERIES_NUC980) */

