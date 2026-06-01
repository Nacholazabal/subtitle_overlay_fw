/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef HDMI_VDMA_UAPI_H_
#define HDMI_VDMA_UAPI_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define HDMI_VDMA_DEVICE_NAME "hdmi-vdma"
#define HDMI_VDMA_FRAME_COUNT 3U

struct hdmi_vdma_info
{
    __u32 frame_count;
    __u32 frame_size;
    __u32 max_width;
    __u32 max_height;
    __u32 max_stride;
};

struct hdmi_vdma_config
{
    __u32 width;
    __u32 height;
    __u32 stride;
    __u32 frame_index;
};

struct hdmi_vdma_channel_status
{
    __u32 running;
    __u32 frame_index;
    __u32 last_cookie;
    __u32 dma_status;
};

struct hdmi_vdma_status
{
    struct hdmi_vdma_channel_status mm2s;
    struct hdmi_vdma_channel_status s2mm;
};

#define HDMI_VDMA_IOC_MAGIC 'V'

#define HDMI_VDMA_GET_INFO _IOR(HDMI_VDMA_IOC_MAGIC, 0x00, struct hdmi_vdma_info)

#define HDMI_VDMA_MM2S_CONFIGURE _IOW(HDMI_VDMA_IOC_MAGIC, 0x10, struct hdmi_vdma_config)
#define HDMI_VDMA_MM2S_START     _IO(HDMI_VDMA_IOC_MAGIC, 0x11)
#define HDMI_VDMA_MM2S_STOP      _IO(HDMI_VDMA_IOC_MAGIC, 0x12)
#define HDMI_VDMA_MM2S_SELECT    _IOW(HDMI_VDMA_IOC_MAGIC, 0x13, __u32)
#define HDMI_VDMA_MM2S_STATUS    _IOR(HDMI_VDMA_IOC_MAGIC, 0x14, struct hdmi_vdma_channel_status)

#define HDMI_VDMA_S2MM_CONFIGURE _IOW(HDMI_VDMA_IOC_MAGIC, 0x20, struct hdmi_vdma_config)
#define HDMI_VDMA_S2MM_START     _IO(HDMI_VDMA_IOC_MAGIC, 0x21)
#define HDMI_VDMA_S2MM_STOP      _IO(HDMI_VDMA_IOC_MAGIC, 0x22)
#define HDMI_VDMA_S2MM_SELECT    _IOW(HDMI_VDMA_IOC_MAGIC, 0x23, __u32)
#define HDMI_VDMA_S2MM_STATUS    _IOR(HDMI_VDMA_IOC_MAGIC, 0x24, struct hdmi_vdma_channel_status)

#define HDMI_VDMA_GET_STATUS _IOR(HDMI_VDMA_IOC_MAGIC, 0x30, struct hdmi_vdma_status)

#endif /* HDMI_VDMA_UAPI_H_ */
