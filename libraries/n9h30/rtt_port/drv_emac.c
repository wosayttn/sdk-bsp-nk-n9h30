/**************************************************************************//**
*
* @copyright (C) 2020 Nuvoton Technology Corp. All rights reserved.
*
* SPDX-License-Identifier: Apache-2.0
*
* Change Logs:
* Date            Author           Notes
* 2020-12-12      Wayne            First version
*
******************************************************************************/

#include <rtconfig.h>

#if defined(BSP_USING_EMAC) && defined(RT_USING_LWIP)

#include <rtdevice.h>
#include "NuMicro.h"
#include <netif/ethernetif.h>
#include <netif/etharp.h>
#include <lwip/icmp.h>
#include <lwip/pbuf.h>
#include <lwip/sys.h>
#include "lwipopts.h"

#include "drv_sys.h"
#include "drv_emac.h"

/* Private define ---------------------------------------------------------------*/
// RT_DEV_NAME_PREFIX e

#define NU_EMAC_DEBUG
#if defined(NU_EMAC_DEBUG)
    //#define NU_EMAC_RX_DUMP
    //#define NU_EMAC_TX_DUMP
    #define NU_EMAC_TRACE         rt_kprintf
#else
    #define NU_EMAC_TRACE(...)
#endif

#define NU_EMAC_TID_STACK_SIZE  2048

struct nu_emac
{
    struct eth_device   eth;
    char               *name;
    EMAC_MEMMGR_T       memmgr;
    IRQn_Type           irqn_tx;
    IRQn_Type           irqn_rx;
    E_SYS_IPRST         rstidx;
    E_SYS_IPCLK         clkidx;
    rt_thread_t         link_monitor;
    rt_uint8_t          mac_addr[8];
};
typedef struct nu_emac *nu_emac_t;

/* Private functions ------------------------------------------------------------*/
#if defined(NU_EMAC_RX_DUMP) || defined(NU_EMAC_TX_DUMP)
    static void nu_emac_pkt_dump(const char *msg, const struct pbuf *p);
#endif
#if LWIP_IPV4 && LWIP_IGMP
    static err_t nu_igmp_mac_filter(struct netif *netif, const ip4_addr_t *ip4_addr, enum netif_mac_filter_action action);
#endif
static void nu_emac_halt(nu_emac_t);
static void nu_emac_reinit(nu_emac_t);
static void link_monitor(void *param);
static rt_err_t nu_emac_init(rt_device_t dev);
static rt_err_t nu_emac_control(rt_device_t dev, int cmd, void *args);
static rt_err_t nu_emac_open(rt_device_t dev, rt_uint16_t oflag);
static rt_err_t nu_emac_close(rt_device_t dev);
static rt_size_t nu_emac_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
static rt_size_t nu_emac_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size);
static rt_err_t nu_emac_tx(rt_device_t dev, struct pbuf *p);
static struct pbuf *nu_emac_rx(rt_device_t dev);
static void rt_hw_nu_emac_assign_macaddr(nu_emac_t psNuEMAC);
static int rt_hw_nu_emac_init(void);
static void *nu_emac_memcpy(void *dest, void *src, unsigned int count);
static void nu_emac_tx_isr(int vector, void *param);
static void nu_emac_rx_isr(int vector, void *param);

/* Private variables ------------------------------------------------------------*/
static struct nu_emac nu_emac_arr[] =
{
#if defined(BSP_USING_EMAC0)
    {
        .name            =  "e0",
        .memmgr.psEmac   = (EMAC_T *)EMC0_BA,
        .irqn_tx         =  IRQ_EMC0_TX,
        .irqn_rx         =  IRQ_EMC0_RX,
        .rstidx          =  EMAC0RST,
        .clkidx          =  EMAC0CKEN,
        .mac_addr        = {0x82, 0x06, 0x21, 0x94, 0x53, 0x01},
    },
#endif
#if defined(BSP_USING_EMAC1)
    {
        .name           =  "e1",
        .memmgr.psEmac  = (EMAC_T *)EMC1_BA,
        .irqn_tx        =  IRQ_EMC1_TX,
        .irqn_rx        =  IRQ_EMC1_RX,
        .rstidx         =  EMAC1RST,
        .clkidx         =  EMAC1CKEN,
        .mac_addr        = {0x82, 0x06, 0x21, 0x94, 0x53, 0x02},
    },
#endif
};

#if defined(NU_EMAC_RX_DUMP) ||  defined(NU_EMAC_TX_DUMP)
static void nu_emac_pkt_dump(const char *msg, const struct pbuf *p)
{
    rt_uint32_t i;
    rt_uint8_t *ptr = p->payload;

    NU_EMAC_TRACE("%s %d byte\n", msg, p->tot_len);

    for (i = 0; i < p->tot_len; i++)
    {
        if ((i % 8) == 0)
        {
            NU_EMAC_TRACE("  ");
        }
        if ((i % 16) == 0)
        {
            NU_EMAC_TRACE("\r\n");
        }
        NU_EMAC_TRACE("%02x ", *ptr);
        ptr++;
    }
    NU_EMAC_TRACE("\n\n");
}
#endif /* dump */

static void nu_emac_halt(nu_emac_t psNuEmac)
{
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;
    EMAC_DISABLE_RX(EMAC);
    EMAC_DISABLE_TX(EMAC);
}

static void *nu_emac_memcpy(void *dest, void *src, unsigned int count)
{
    return rt_memcpy(dest, src, count);
}

static void nu_emac_reinit(nu_emac_t psNuEmac)
{
    rt_uint32_t EMAC_CAMxM[EMAC_CAMENTRY_NB];
    rt_uint32_t EMAC_CAMxL[EMAC_CAMENTRY_NB];
    rt_uint32_t EMAC_CAMEN;
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;

    // Backup MAC address.
    EMAC_CAMEN = EMAC->CAMEN;
    for (rt_uint8_t index = 0 ; index < EMAC_CAMENTRY_NB; index ++)
    {
        rt_uint32_t *CAMxM = (rt_uint32_t *)((rt_uint32_t)&EMAC->CAM0M + (index * 8));
        rt_uint32_t *CAMxL = (rt_uint32_t *)((rt_uint32_t)&EMAC->CAM0L + (index * 8));

        EMAC_CAMxM[index] = *CAMxM;
        EMAC_CAMxL[index] = *CAMxL;
    }

    nu_emac_halt(psNuEmac);
    EMAC_Close(EMAC);
    EMAC_Open(&psNuEmac->memmgr, (uint8_t *)&psNuEmac->mac_addr[0]);
    EMAC_ENABLE_TX(EMAC);
    EMAC_ENABLE_RX(EMAC);

    // Restore MAC address.
    for (rt_uint8_t index = 0 ; index < EMAC_CAMENTRY_NB; index ++)
    {
        rt_uint32_t *CAMxM = (rt_uint32_t *)((rt_uint32_t)&EMAC->CAM0M + (index * 8));
        rt_uint32_t *CAMxL = (rt_uint32_t *)((rt_uint32_t)&EMAC->CAM0L + (index * 8));

        *CAMxM = EMAC_CAMxM[index];
        *CAMxL = EMAC_CAMxL[index];
    }
    EMAC->CAMEN = EMAC_CAMEN;
}

#if LWIP_IPV4 && LWIP_IGMP
static err_t nu_igmp_mac_filter(struct netif *netif, const ip4_addr_t *ip4_addr, enum netif_mac_filter_action action)
{
    nu_emac_t psNuEmac = (nu_emac_t)netif->state;
    rt_uint8_t mac[6];
    int32_t ret = 0;
    const uint8_t *p = (const uint8_t *)ip4_addr;

    mac[0] = 0x01;
    mac[1] = 0x00;
    mac[2] = 0x5E;
    mac[3] = *(p + 1) & 0x7F;
    mac[4] = *(p + 2);
    mac[5] = *(p + 3);

    ret = EMAC_FillCamEntry(psNuEmac->memmgr.psEmac, (uint8_t *)&mac[0]);
    if (ret >= 0)
    {
        NU_EMAC_TRACE("%s %s %s ", __FUNCTION__, (action == NETIF_ADD_MAC_FILTER) ? "add" : "del", ip4addr_ntoa(ip4_addr));
        NU_EMAC_TRACE("%02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    return (ret >= 0) ? RT_EOK : -(RT_ERROR);
}
#endif /* LWIP_IPV4 && LWIP_IGMP */

static void link_monitor(void *param)
{
    nu_emac_t psNuEmac = (nu_emac_t)param;
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;
    uint32_t LinkStatus_Last = EMAC_LINK_DOWN;

    EMAC_PhyInit(EMAC);

    while (1)
    {
        uint32_t LinkStatus_Current = EMAC_CheckLinkStatus(EMAC);
        /* linkchange */
        if (LinkStatus_Last != LinkStatus_Current)
        {

            switch (LinkStatus_Current)
            {
            case EMAC_LINK_DOWN:
                NU_EMAC_TRACE("[%s] Link status: Down\n", psNuEmac->name);
                break;

            case EMAC_LINK_100F:
                NU_EMAC_TRACE("[%s] Link status: 100F\n", psNuEmac->name);
                break;

            case EMAC_LINK_100H:
                NU_EMAC_TRACE("[%s] Link status: 100H\n", psNuEmac->name);
                break;

            case EMAC_LINK_10F:
                NU_EMAC_TRACE("[%s] Link status: 10F\n", psNuEmac->name);
                break;

            case EMAC_LINK_10H:
                NU_EMAC_TRACE("[%s] Link status: 10H\n", psNuEmac->name);
                break;
            } /* switch( LinkStatus_Current ) */

            /* Send link status to upper layer. */
            if (LinkStatus_Current == EMAC_LINK_DOWN)
            {
                eth_device_linkchange(&psNuEmac->eth, RT_FALSE);
            }
            else
            {
                eth_device_linkchange(&psNuEmac->eth, RT_TRUE);
            }
            LinkStatus_Last = LinkStatus_Current;

        } /* if ( LinkStatus_Last != LinkStatus_Current ) */

        rt_thread_delay(RT_TICK_PER_SECOND);

    } /* while(1) */

}

static void nu_memmgr_init(EMAC_MEMMGR_T *psMemMgr)
{
    psMemMgr->u32TxDescSize = EMAC_TX_DESC_SIZE;
    psMemMgr->u32RxDescSize = EMAC_RX_DESC_SIZE;

    psMemMgr->psTXDescs = (EMAC_DESCRIPTOR_T *) rt_malloc_align(sizeof(EMAC_DESCRIPTOR_T) * psMemMgr->u32TxDescSize, 32);
    RT_ASSERT(psMemMgr->psTXDescs != RT_NULL);

    psMemMgr->psRXDescs = (EMAC_DESCRIPTOR_T *) rt_malloc_align(sizeof(EMAC_DESCRIPTOR_T) * psMemMgr->u32RxDescSize, 32);
    RT_ASSERT(psMemMgr->psRXDescs != RT_NULL);

    psMemMgr->psTXFrames = (EMAC_FRAME_T *) rt_malloc_align(sizeof(EMAC_FRAME_T) * psMemMgr->u32TxDescSize, 32);
    RT_ASSERT(psMemMgr->psTXFrames != RT_NULL);

    psMemMgr->psRXFrames = (EMAC_FRAME_T *) rt_malloc_align(sizeof(EMAC_FRAME_T) * psMemMgr->u32RxDescSize, 32);
    RT_ASSERT(psMemMgr->psRXFrames != RT_NULL);

    NU_EMAC_TRACE("[%s] TxDesc num: %d, RxDesc num: %d\n", __func__, EMAC_TX_DESC_SIZE, EMAC_RX_DESC_SIZE);
}

static rt_err_t nu_emac_init(rt_device_t dev)
{
    nu_emac_t psNuEmac = (nu_emac_t)dev;
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;
    char szTmp[16];
    rt_err_t ret = RT_EOK;

    nu_memmgr_init(&psNuEmac->memmgr);

    snprintf(szTmp, sizeof(szTmp), "%sphy", psNuEmac->name);

    EMAC_Reset(EMAC);

    EMAC_Close(EMAC);
    EMAC_Open(&psNuEmac->memmgr, (uint8_t *)&psNuEmac->mac_addr[0]);

#if defined(BSP_USING_MMU)
    mmu_clean_invalidated_dcache((uint32_t)psNuEmac->memmgr.psTXDescs, sizeof(EMAC_DESCRIPTOR_T)*psNuEmac->memmgr.u32TxDescSize);
    mmu_clean_invalidated_dcache((uint32_t)psNuEmac->memmgr.psRXDescs, sizeof(EMAC_DESCRIPTOR_T)*psNuEmac->memmgr.u32RxDescSize);
#endif

    EMAC_ENABLE_RX(EMAC);
    EMAC_ENABLE_TX(EMAC);

    EMAC_TRIGGER_RX(EMAC);

#if defined(LWIP_IPV4) && defined(LWIP_IGMP)
    netif_set_igmp_mac_filter(psNuEmac->eth.netif, nu_igmp_mac_filter);
#endif /* LWIP_IPV4 && LWIP_IGMP */

    psNuEmac->link_monitor =  rt_thread_create((const char *)szTmp,
                              link_monitor,
                              (void *)psNuEmac,
                              NU_EMAC_TID_STACK_SIZE,
                              RT_THREAD_PRIORITY_MAX - 2,
                              10);
    RT_ASSERT(psNuEmac->link_monitor != RT_NULL);

    ret = rt_thread_startup(psNuEmac->link_monitor);
    RT_ASSERT(ret == RT_EOK);

    return RT_EOK;
}

static rt_err_t nu_emac_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t nu_emac_close(rt_device_t dev)
{
    return RT_EOK;
}

static rt_size_t nu_emac_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_set_errno(-RT_ENOSYS);
    return 0;
}

static rt_size_t nu_emac_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    rt_set_errno(-RT_ENOSYS);
    return 0;
}

static rt_err_t nu_emac_control(rt_device_t dev, int cmd, void *args)
{
    nu_emac_t psNuEMAC = (nu_emac_t)dev;
    switch (cmd)
    {
    case NIOCTL_GADDR:
        /* Get MAC address */
        if (args)
            rt_memcpy(args, &psNuEMAC->mac_addr[0], NETIF_MAX_HWADDR_LEN);
        else
            return -RT_ERROR;

        break;

    case NIOCTL_PRIV_CHANGE_MAC:
    {
        uint8_t *pu8MacAddr = (uint8_t *)args;
        if (pu8MacAddr)
        {
            EMAC_T *EMAC = psNuEMAC->memmgr.psEmac;
            rt_memcpy(&psNuEMAC->mac_addr[0], pu8MacAddr, NETIF_MAX_HWADDR_LEN);
            EMAC_SetMacAddr(EMAC, psNuEMAC->mac_addr);
            if (psNuEMAC->eth.netif)
            {
                struct netif *netif = psNuEMAC->eth.netif;
                rt_memcpy(&netif->hwaddr[0], &psNuEMAC->mac_addr[0], NETIF_MAX_HWADDR_LEN);
            }
        }

        NU_EMAC_TRACE("[%s] New MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n", \
                      psNuEMAC->name, \
                      psNuEMAC->mac_addr[0], \
                      psNuEMAC->mac_addr[1], \
                      psNuEMAC->mac_addr[2], \
                      psNuEMAC->mac_addr[3], \
                      psNuEMAC->mac_addr[4], \
                      psNuEMAC->mac_addr[5]);
    }
    break;

    default :
        break;
    }

    return RT_EOK;
}

static rt_err_t nu_emac_tx(rt_device_t dev, struct pbuf *p)
{
    nu_emac_t psNuEmac = (nu_emac_t)dev;
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;
    struct pbuf *q;
    rt_uint32_t offset = 0;
    rt_uint8_t *buf;

    buf = (rt_uint8_t *)EMAC_ClaimFreeTXBuf(&psNuEmac->memmgr);
    /* Get free TX buffer */
    if (buf == RT_NULL)
        return -RT_ERROR;

    for (q = p; q != NULL; q = q->next)
    {
        rt_uint8_t *ptr;
        rt_uint32_t len;

        len = q->len;
        ptr = q->payload;

        nu_emac_memcpy(&buf[offset], ptr, len);

        offset += len;
    }

#if defined(NU_EMAC_TX_DUMP)
    nu_emac_pkt_dump("TX dump", p);
#endif

    if (!EMAC_SendPktWoCopy(&psNuEmac->memmgr, offset))
        return -RT_ERROR;

    EMAC_SendPktDone(&psNuEmac->memmgr);

    /* Return SUCCESS? */
    return  RT_EOK;
}

static void eth_rx_push(nu_emac_t psNuEmac)
{
    struct netif *netif = psNuEmac->eth.netif;
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;

    while (1)
    {
        err_t ret;
        struct pbuf *pbuf = RT_NULL;
        uint8_t *pu8DataBuf = NULL;
        int s32PktLen;

        if ((s32PktLen = EMAC_GetAvailRXBufSize(&psNuEmac->memmgr, &pu8DataBuf)) <= 0)
        {
            break;
        }
        /* Get current RX descriptor. */
        EMAC_DESCRIPTOR_T *cur_rx = EMAC_RecvPktDoneWoRxTrigger(&psNuEmac->memmgr);

        /* Allocate a pbuf chain of pbufs from the pool. */
        if ((pbuf = pbuf_alloc(PBUF_RAW, s32PktLen, PBUF_POOL)) != NULL)
        {
#if defined(BSP_USING_MMU)
            mmu_invalidate_dcache((rt_uint32_t)pu8DataBuf, (rt_uint32_t)s32PktLen);
#endif
            pbuf_take(pbuf, pu8DataBuf, s32PktLen);
        }
        else
        {
            rt_kprintf("[%s] drop the packet %08x\n", psNuEmac->name, pu8DataBuf);
        }

        /* Free or drop descriptor. */
        EMAC_RxTrigger(&psNuEmac->memmgr, cur_rx);

        if ((ret = netif->input(pbuf, netif)) != ERR_OK)
        {
            rt_kprintf("[%s] input error %08x err_t:%08x\n", psNuEmac->name, pbuf, ret);
            pbuf_free(pbuf);
            break;
        }

    } //while(1)

    /* Trigger EMAC for receiving. */
    EMAC_TRIGGER_RX(EMAC);
}

static void nu_emac_rx_isr(int vector, void *param)
{
    nu_emac_t psNuEmac = (nu_emac_t)param;
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;

    uint32_t u32INTSTS = EMAC->INTSTS & 0xFFFF;

    if (u32INTSTS & EMAC_INTSTS_RDUIF_Msk)
    {
        /* No RX descriptor available, we need to get data from RX pool */
        NU_EMAC_TRACE("No RX descriptor available, INTEN=%08x, INTSTS=%08x\n", EMAC->INTEN, EMAC->INTSTS);
        EMAC_CLEAR_INT_FLAG(EMAC, (EMAC_INTSTS_RDUIF_Msk | EMAC_INTSTS_RXGDIF_Msk));
        u32INTSTS &= ~(EMAC_INTSTS_RDUIF_Msk | EMAC_INTSTS_RXGDIF_Msk);

        eth_rx_push(psNuEmac);
    }
    else if (u32INTSTS & EMAC_INTSTS_RXGDIF_Msk)
    {
        EMAC_CLEAR_INT_FLAG(EMAC, (EMAC_INTSTS_RXGDIF_Msk));
        u32INTSTS &= ~EMAC_INTSTS_RXGDIF_Msk;

        /* A good packet ready. */
        eth_rx_push(psNuEmac);
    }

    /* Receive Bus Error Interrupt */
    if (u32INTSTS & EMAC_INTSTS_RXBEIF_Msk)
    {
        NU_EMAC_TRACE("Reinit Rx EMAC\n");
        EMAC_CLEAR_INT_FLAG(EMAC, (EMAC_INTSTS_RXBEIF_Msk));
        u32INTSTS &= ~EMAC_INTSTS_RXBEIF_Msk;

        nu_emac_reinit(psNuEmac);
    }

    EMAC_CLEAR_INT_FLAG(EMAC, u32INTSTS);
}

static void nu_emac_tx_isr(int vector, void *param)
{
    nu_emac_t psNuEmac = (nu_emac_t)param;
    EMAC_T *EMAC = psNuEmac->memmgr.psEmac;
    rt_err_t result = RT_EOK;

    uint32_t u32INTSTS = EMAC->INTSTS & 0xFFFF0000;

    /* Wake-up suspended process to send */
    if (u32INTSTS & EMAC_INTSTS_TXCPIF_Msk)
    {
        EMAC_CLEAR_INT_FLAG(EMAC, (EMAC_INTSTS_TXCPIF_Msk));
        u32INTSTS &= ~EMAC_INTSTS_TXCPIF_Msk;
    }

    if (u32INTSTS & EMAC_INTSTS_TXBEIF_Msk)
    {
        NU_EMAC_TRACE("Reinit Tx EMAC\n");
        EMAC_CLEAR_INT_FLAG(EMAC, (EMAC_INTSTS_TXBEIF_Msk));
        u32INTSTS &= ~EMAC_INTSTS_TXBEIF_Msk;

        nu_emac_reinit(psNuEmac);
    }

    EMAC_CLEAR_INT_FLAG(EMAC, u32INTSTS);
}

static int rt_hw_nu_emac_init(void)
{
    int i;
    rt_err_t ret = RT_EOK;
    char szTmp[32];

    /* MDC CLK divider */
    outpw(REG_CLK_DIVCTL8, (inpw(REG_CLK_DIVCTL8) & ~0xFF) | 0xA0);

    for (i = (EMAC_START + 1); i < EMAC_CNT; i++)
    {
        nu_emac_t psNuEMAC = (nu_emac_t)&nu_emac_arr[i];

        nu_sys_ipclk_enable(psNuEMAC->clkidx);

        nu_sys_ip_reset(psNuEMAC->rstidx);

        /* Register member functions */
        psNuEMAC->eth.parent.init       = nu_emac_init;
        psNuEMAC->eth.parent.open       = nu_emac_open;
        psNuEMAC->eth.parent.close      = nu_emac_close;
        psNuEMAC->eth.parent.read       = nu_emac_read;
        psNuEMAC->eth.parent.write      = nu_emac_write;
        psNuEMAC->eth.parent.control    = nu_emac_control;
        psNuEMAC->eth.parent.user_data  = psNuEMAC;
        psNuEMAC->eth.eth_rx            = RT_NULL;
        psNuEMAC->eth.eth_tx            = nu_emac_tx;

        snprintf(szTmp, sizeof(szTmp), "%s_tx", psNuEMAC->name);
        rt_hw_interrupt_install(psNuEMAC->irqn_tx, nu_emac_tx_isr, (void *)psNuEMAC, szTmp);
        rt_hw_interrupt_umask(psNuEMAC->irqn_tx);

        snprintf(szTmp, sizeof(szTmp), "%s_rx", psNuEMAC->name);
        rt_hw_interrupt_install(psNuEMAC->irqn_rx, nu_emac_rx_isr, (void *)psNuEMAC, szTmp);
        rt_hw_interrupt_umask(psNuEMAC->irqn_rx);

        /* Register eth device */
        ret = eth_device_init(&psNuEMAC->eth, psNuEMAC->name);
        RT_ASSERT(ret == RT_EOK);
    }

    return 0;
}

INIT_APP_EXPORT(rt_hw_nu_emac_init);

int nu_emac_assign_mac_addr(EMAC_IDX evIdx, uint8_t *pu8MacAddr)
{
    rt_memcpy(nu_emac_arr[evIdx].mac_addr, pu8MacAddr, NETIF_MAX_HWADDR_LEN);
    return 0;
}

#if 1
/*
    Remeber src += lwipiperf_SRCS in components\net\lwip\lwip-*\SConscript
*/
#include "lwip/apps/lwiperf.h"

static void
lwiperf_report(void *arg, enum lwiperf_report_type report_type,
               const ip_addr_t *local_addr, u16_t local_port, const ip_addr_t *remote_addr, u16_t remote_port,
               u32_t bytes_transferred, u32_t ms_duration, u32_t bandwidth_kbitpsec)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(local_addr);
    LWIP_UNUSED_ARG(local_port);

    NU_EMAC_TRACE("IPERF report: type=%d, remote: %s:%d, total bytes: %"U32_F", duration in ms: %"U32_F", kbits/s: %"U32_F"\n",
                  (int)report_type, ipaddr_ntoa(remote_addr), (int)remote_port, bytes_transferred, ms_duration, bandwidth_kbitpsec);
}

int lwiperf_example_init(void)
{
    lwiperf_start_tcp_server_default(lwiperf_report, NULL);

    return 0;
}
INIT_APP_EXPORT(lwiperf_example_init);
MSH_CMD_EXPORT(lwiperf_example_init, start lwip tcp server);
#endif

#endif /* #if defined( BSP_USING_EMAC ) && defined( RT_USING_LWIP )*/
