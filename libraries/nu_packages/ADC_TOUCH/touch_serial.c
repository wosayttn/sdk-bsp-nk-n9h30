/**************************************************************************//**
* @copyright (C) 2020 Nuvoton Technology Corp. All rights reserved.
*
* SPDX-License-Identifier: Apache-2.0
*
* Change Logs:
* Date            Author       Notes
* 2023-02-20      Wayne        First version
*
******************************************************************************/

#include <rtconfig.h>

#if defined(NU_PKG_USING_ADC_TOUCH_SERIAL)

#include <rtthread.h>
#include "rtdevice.h"
#include "touch.h"

/* Private define ---------------------------------------------------------------*/
#define DEF_ADC_TOUCH_SMPL_TICK  40
#define TOUCH_MQ_LENGTH   32

#define SERIO_SERTH 0x3f
#define SERTH_FORMAT_MAX_LENGTH 6
#define SERTH_RESPONSE_BEGIN_BYTE 0x80
#define SERTH_FORMAT_PRESSURE_BIT 0x40
#define SERTH_FORMAT_TOUCH_BIT 0x01
#define SERTH_FORMAT_RESOLUTION 0x06

#define SERTH_MIN_XC 0
#define SERTH_MAX_XC 0x4000
#define SERTH_MIN_YC 0
#define SERTH_MAX_YC 0x4000

#define SERTH_GET_XC(data, resbits, shift) (((data[2] & (resbits)) << 7) + (data[1] & 0x7f))
#define SERTH_GET_YC(data, resbits, shift) (((data[4] & (resbits)) << 7) + (data[3] & 0x7f))
#define SERTH_GET_PRESSURED(data) (SERTH_FORMAT_PRESSURE_BIT & data[0])
#define SERTH_GET_TOUCHED(data) (SERTH_FORMAT_TOUCH_BIT & data[0])

/* Private Typedef --------------------------------------------------------------*/
struct nu_adc_touch_data
{
    uint32_t    u32X;
    uint32_t    u32Y;
    uint32_t    u32Z0;
    uint32_t    u32Z1;
};
typedef struct nu_adc_touch_data *nu_adc_touch_data_t;

struct nu_serial_touch
{
    struct rt_touch_device dev;
    rt_device_t serial;
    rt_touch_t  touch;

    int32_t x_range;
    int32_t y_range;

    struct nu_adc_touch_data tp_data;
    rt_tick_t last_point;
    rt_tick_t last_up;

    int32_t idx;
    uint8_t data[SERTH_FORMAT_MAX_LENGTH + 2];
};
typedef struct nu_serial_touch *nu_serial_touch_t;

static rt_mq_t    g_pmqTouchXYZ;
static struct nu_serial_touch s_NuSerialTouch = {0};
static struct serial_configure sSerialTouchConfig = RT_SERIAL_CONFIG_DEFAULT;
static rt_sem_t serial_touch_sem = RT_NULL;

static rt_err_t serial_touch_rx_done(rt_device_t dev, rt_size_t size)
{
    return rt_sem_release(serial_touch_sem);
}

static rt_err_t serial_touch_process(uint8_t *buffer, int len)
{
    int i = 0;

    while (i < len)
    {
        s_NuSerialTouch.data[s_NuSerialTouch.idx] = buffer[i];
        if (SERTH_RESPONSE_BEGIN_BYTE & s_NuSerialTouch.data[0]) //=0x80 ?
        {
            if (++s_NuSerialTouch.idx == 5)
            {
                struct nu_adc_touch_data *pTPData = &s_NuSerialTouch.tp_data;

                pTPData->u32X  = (uint16_t)SERTH_GET_XC(s_NuSerialTouch.data, 0x1f, 0);
                pTPData->u32Y  = (uint16_t)SERTH_GET_YC(s_NuSerialTouch.data, 0x1f, 0);
                pTPData->u32Z0 = (SERTH_GET_TOUCHED(s_NuSerialTouch.data) || SERTH_GET_PRESSURED(s_NuSerialTouch.data)) ? 1000 : 0;
                pTPData->u32Z1 = pTPData->u32Z0;

                //rt_kprintf("[%02X] -> ", s_NuSerialTouch.data[0]);
                //rt_kprintf("(%d, %d, %d) -> \n", pTPData->u32X, pTPData->u32Y, pTPData->u32Z0);

                if (rt_mq_send(g_pmqTouchXYZ, (const void *)pTPData, sizeof(struct nu_adc_touch_data)) == RT_EOK)
                {
                    s_NuSerialTouch.last_point = rt_tick_get();

                    if (pTPData->u32Z0 == 0)
                        s_NuSerialTouch.last_up = rt_tick_get();

                    if (s_NuSerialTouch.touch != RT_NULL)
                        rt_hw_touch_isr(s_NuSerialTouch.touch);
                }

                s_NuSerialTouch.idx = 0;
            }
        }
        else
            rt_kprintf("unknown data: %02x\n", s_NuSerialTouch.data[0]);

        i++;
    }

    //rt_kprintf("\n");

    return RT_EOK;
}

static void serial_touch_entry(void *parameter)
{
    rt_device_t serial;
    rt_err_t ret;
    uint8_t read_buf[256];
    int len;

    serial_touch_sem = rt_sem_create("touch_sem", 0, RT_IPC_FLAG_FIFO);
    RT_ASSERT(serial_touch_sem);

    serial = rt_device_find(NU_PKG_SERIAL_TOUCH_DEVNAME);
    RT_ASSERT(serial);

    sSerialTouchConfig.baud_rate = 9600;
    ret = rt_device_control(serial, RT_DEVICE_CTRL_CONFIG, &sSerialTouchConfig);
    RT_ASSERT(ret == RT_EOK);

    /* Set rx indicate function */
    ret = rt_device_set_rx_indicate(serial, serial_touch_rx_done);
    RT_ASSERT(ret == RT_EOK);

    ret = rt_device_open(serial, RT_DEVICE_FLAG_INT_RX);
    RT_ASSERT(ret == RT_EOK);

    s_NuSerialTouch.serial = serial;

    while (1)
    {
#define DEF_UART_POLLING_T    200
        if ((ret = rt_sem_take(serial_touch_sem, DEF_UART_POLLING_T)) == RT_EOK)
        {
            if ((len = rt_device_read(serial, 0, &read_buf[0], sizeof(read_buf))) > 0)
            {
                serial_touch_process(read_buf, len);
            }
        }
        else if ((s_NuSerialTouch.last_point - s_NuSerialTouch.last_up) >= DEF_UART_POLLING_T)
        {
            /* HW issue: The serial touch can't report last up event. */
            /* Avoid UP-event missing, we send a up event. */

            struct nu_adc_touch_data *pTPData = &s_NuSerialTouch.tp_data;

            pTPData->u32Z0 = 0;
            pTPData->u32Z1 = 0;

            if (rt_mq_send(g_pmqTouchXYZ, (const void *)pTPData, sizeof(struct nu_adc_touch_data)) == RT_EOK)
            {
                if (s_NuSerialTouch.touch != RT_NULL)
                    rt_hw_touch_isr(s_NuSerialTouch.touch);
            }

            s_NuSerialTouch.last_up = s_NuSerialTouch.last_point;

            rt_kprintf("[Serial touch] Re-send up-event done.\n");
        }
    }

    ret = rt_device_close(serial);
    RT_ASSERT(ret == RT_EOK);
}

int32_t nu_adc_touch_read_xyz(uint32_t *bufX, uint32_t *bufY, uint32_t *bufZ0, uint32_t *bufZ1, int32_t dataCnt)
{
    int i;
    struct nu_adc_touch_data value;

    for (i = 0 ; i < dataCnt; i++)
    {
        if (rt_mq_recv(g_pmqTouchXYZ, (void *)&value, sizeof(struct nu_adc_touch_data), 0) == -RT_ETIMEOUT)
            break;

        bufX[i]  = value.u32X;
        bufY[i]  = value.u32Y;
        bufZ0[i] = value.u32Z0;
        bufZ1[i] = value.u32Z1;
    }
    return i;
}


void nu_adc_touch_detect(rt_bool_t bStartDetect)
{
}

rt_err_t nu_adc_touch_enable(rt_touch_t psRtTouch)
{
    s_NuSerialTouch.touch = psRtTouch;

    return RT_EOK;
}

rt_err_t nu_adc_touch_disable(void)
{
    s_NuSerialTouch.touch = RT_NULL;

    return RT_EOK;
}

int nu_adc_touch_serial_register(void)
{
    rt_thread_t  serial_touch_thread;

    s_NuSerialTouch.serial = rt_device_find(NU_PKG_SERIAL_TOUCH_DEVNAME);
    RT_ASSERT(s_NuSerialTouch.serial);

    g_pmqTouchXYZ = rt_mq_create("ADC_TOUCH_SERIAL", sizeof(struct nu_adc_touch_data), TOUCH_MQ_LENGTH, RT_IPC_FLAG_FIFO);
    RT_ASSERT(g_pmqTouchXYZ);

    serial_touch_thread = rt_thread_create("serial_touch_thread",
                                           serial_touch_entry,
                                           RT_NULL,
                                           2048,
                                           5,
                                           5);
    if (serial_touch_thread != RT_NULL)
        rt_thread_startup(serial_touch_thread);

    return RT_EOK;
}
INIT_COMPONENT_EXPORT(nu_adc_touch_serial_register);

#endif //#if defined(NU_PKG_USING_ADC_TOUCH_SERIAL)
