// SPDX-License-Identifier: GPL-2.0
/*
 * hdmi_vdma_client.c - dmaengine client for AXI VDMA MM2S + S2MM.
 *
 * The Linux xilinx-vdma driver owns the hardware registers. This module owns
 * the application-facing DMA buffers and submits display/capture transfers.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "hdmi_vdma.h"

#define HDMI_VDMA_DRIVER_VERSION "v0.1 mm2s+s2mm passthrough build"

#define HDMI_VDMA_MAX_WIDTH      1920U
#define HDMI_VDMA_MAX_HEIGHT     1080U
#define HDMI_VDMA_BPP            3U
#define HDMI_VDMA_MAX_STRIDE     (HDMI_VDMA_MAX_WIDTH * HDMI_VDMA_BPP)
#define HDMI_VDMA_FRAME_BYTES    (HDMI_VDMA_MAX_STRIDE * HDMI_VDMA_MAX_HEIGHT)
#define HDMI_VDMA_FRAME_MAP_SIZE PAGE_ALIGN(HDMI_VDMA_FRAME_BYTES)

struct hdmi_vdma_channel
{
    struct dma_chan* chan;
    struct hdmi_vdma_config cfg;
    dma_cookie_t cookie;
    bool configured;
    bool running;
    enum dma_transfer_direction dir;
    const char* name;
};

struct hdmi_vdma_dev
{
    struct device* dev;
    struct hdmi_vdma_channel mm2s;
    struct hdmi_vdma_channel s2mm;
    struct cdev cdev;
    dev_t devt;
    struct class* class;
    struct device* chardev;

    void* frames[HDMI_VDMA_FRAME_COUNT];
    dma_addr_t frames_dma[HDMI_VDMA_FRAME_COUNT];
    size_t total_size;
    bool reserved_mem_attached;
};

static struct hdmi_vdma_dev* g_vdma;

static int hdmi_vdma_validate_config(const struct hdmi_vdma_config* cfg)
{
    if (cfg->width == 0 || cfg->height == 0 || cfg->width > HDMI_VDMA_MAX_WIDTH
        || cfg->height > HDMI_VDMA_MAX_HEIGHT || cfg->stride < cfg->width * HDMI_VDMA_BPP
        || cfg->stride > HDMI_VDMA_MAX_STRIDE || cfg->frame_index >= HDMI_VDMA_FRAME_COUNT)
    {
        return -EINVAL;
    }

    return 0;
}

static void hdmi_vdma_fill_status(struct hdmi_vdma_channel* ch,
                                  struct hdmi_vdma_channel_status* status)
{
    enum dma_status dma_status;

    dma_status = ch->running ? dma_async_is_tx_complete(ch->chan, ch->cookie, NULL, NULL)
                             : DMA_COMPLETE;
    memset(status, 0, sizeof(*status));
    status->running = ch->running ? 1U : 0U;
    status->frame_index = ch->cfg.frame_index;
    status->last_cookie = (u32)ch->cookie;
    status->dma_status = (u32)dma_status;
}

static int hdmi_vdma_submit_locked(struct hdmi_vdma_dev* m, struct hdmi_vdma_channel* ch)
{
    struct dma_interleaved_template* xt;
    struct dma_async_tx_descriptor* desc;
    enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
    dma_addr_t frame_dma;
    size_t xt_size;
    int ret;

    if (!ch->configured)
        return -EINVAL;

    ret = hdmi_vdma_validate_config(&ch->cfg);
    if (ret)
        return ret;

    frame_dma = m->frames_dma[ch->cfg.frame_index];

#ifdef DMA_PREP_REPEAT
    flags |= DMA_PREP_REPEAT;
#endif

    xt_size = sizeof(*xt) + sizeof(struct data_chunk);
    xt = kzalloc(xt_size, GFP_KERNEL);
    if (!xt)
        return -ENOMEM;

    xt->sgl[0].size = ch->cfg.width * HDMI_VDMA_BPP;
    xt->sgl[0].icg = ch->cfg.stride - xt->sgl[0].size;
    xt->dir = ch->dir;
    xt->frame_size = 1;
    xt->numf = ch->cfg.height;

    if (ch->dir == DMA_MEM_TO_DEV)
    {
        xt->src_start = frame_dma;
        xt->src_inc = true;
        xt->dst_inc = false;
    }
    else
    {
        xt->dst_start = frame_dma;
        xt->src_inc = false;
        xt->dst_inc = true;
    }

    desc = dmaengine_prep_interleaved_dma(ch->chan, xt, flags);
    if (!desc)
    {
        dev_err(m->dev, "%s prep_interleaved_dma failed\n", ch->name);
        kfree(xt);
        return -EIO;
    }

    ch->cookie = dmaengine_submit(desc);
    if (dma_submit_error(ch->cookie))
    {
        ret = dma_submit_error(ch->cookie);
        dev_err(m->dev, "%s dmaengine_submit failed: %d\n", ch->name, ret);
        kfree(xt);
        return ret;
    }

    dma_async_issue_pending(ch->chan);
    ch->running = true;
    kfree(xt);
    return 0;
}

static int hdmi_vdma_open(struct inode* inode, struct file* filp)
{
    filp->private_data = g_vdma;
    return g_vdma ? 0 : -ENODEV;
}

static int hdmi_vdma_mmap(struct file* filp, struct vm_area_struct* vma)
{
    struct hdmi_vdma_dev* m = filp->private_data;
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    unsigned int frame_index;

    if (!m || size > HDMI_VDMA_FRAME_MAP_SIZE)
        return -EINVAL;

    if (offset % HDMI_VDMA_FRAME_MAP_SIZE)
        return -EINVAL;

    frame_index = offset / HDMI_VDMA_FRAME_MAP_SIZE;
    if (frame_index >= HDMI_VDMA_FRAME_COUNT || !m->frames[frame_index])
        return -EINVAL;

    vma->vm_pgoff = 0;
    return dma_mmap_coherent(m->dev, vma, m->frames[frame_index], m->frames_dma[frame_index], size);
}

static long hdmi_vdma_channel_ioctl(struct hdmi_vdma_dev* m,
                                    struct hdmi_vdma_channel* ch,
                                    unsigned int cmd,
                                    unsigned long arg,
                                    unsigned int cfg_cmd,
                                    unsigned int start_cmd,
                                    unsigned int stop_cmd,
                                    unsigned int select_cmd,
                                    unsigned int status_cmd)
{
    struct hdmi_vdma_channel_status status;
    struct hdmi_vdma_config cfg;
    u32 frame_index;
    int ret;

    if (cmd == cfg_cmd)
    {
        if (copy_from_user(&cfg, (void __user*)arg, sizeof(cfg)))
            return -EFAULT;
        ret = hdmi_vdma_validate_config(&cfg);
        if (ret)
            return ret;
        ch->cfg = cfg;
        ch->configured = true;
        return 0;
    }

    if (cmd == start_cmd)
    {
        dmaengine_terminate_sync(ch->chan);
        ch->running = false;
        return hdmi_vdma_submit_locked(m, ch);
    }

    if (cmd == stop_cmd)
    {
        dmaengine_terminate_sync(ch->chan);
        ch->running = false;
        return 0;
    }

    if (cmd == select_cmd)
    {
        if (copy_from_user(&frame_index, (void __user*)arg, sizeof(frame_index)))
            return -EFAULT;
        if (frame_index >= HDMI_VDMA_FRAME_COUNT)
            return -EINVAL;
        ch->cfg.frame_index = frame_index;
        if (ch->running)
        {
            dmaengine_terminate_sync(ch->chan);
            ch->running = false;
            return hdmi_vdma_submit_locked(m, ch);
        }
        return 0;
    }

    if (cmd == status_cmd)
    {
        hdmi_vdma_fill_status(ch, &status);
        if (copy_to_user((void __user*)arg, &status, sizeof(status)))
            return -EFAULT;
        return 0;
    }

    return -ENOTTY;
}

static long hdmi_vdma_ioctl(struct file* filp, unsigned int cmd, unsigned long arg)
{
    struct hdmi_vdma_dev* m = filp->private_data;
    struct hdmi_vdma_info info;
    struct hdmi_vdma_status status;
    long ret;

    if (!m)
        return -ENODEV;

    switch (cmd)
    {
    case HDMI_VDMA_GET_INFO:
        memset(&info, 0, sizeof(info));
        info.frame_count = HDMI_VDMA_FRAME_COUNT;
        info.frame_size = HDMI_VDMA_FRAME_MAP_SIZE;
        info.max_width = HDMI_VDMA_MAX_WIDTH;
        info.max_height = HDMI_VDMA_MAX_HEIGHT;
        info.max_stride = HDMI_VDMA_MAX_STRIDE;
        if (copy_to_user((void __user*)arg, &info, sizeof(info)))
            return -EFAULT;
        return 0;

    case HDMI_VDMA_GET_STATUS:
        memset(&status, 0, sizeof(status));
        hdmi_vdma_fill_status(&m->mm2s, &status.mm2s);
        hdmi_vdma_fill_status(&m->s2mm, &status.s2mm);
        if (copy_to_user((void __user*)arg, &status, sizeof(status)))
            return -EFAULT;
        return 0;

    default:
        break;
    }

    ret = hdmi_vdma_channel_ioctl(m,
                                  &m->mm2s,
                                  cmd,
                                  arg,
                                  HDMI_VDMA_MM2S_CONFIGURE,
                                  HDMI_VDMA_MM2S_START,
                                  HDMI_VDMA_MM2S_STOP,
                                  HDMI_VDMA_MM2S_SELECT,
                                  HDMI_VDMA_MM2S_STATUS);
    if (ret != -ENOTTY)
        return ret;

    return hdmi_vdma_channel_ioctl(m,
                                   &m->s2mm,
                                   cmd,
                                   arg,
                                   HDMI_VDMA_S2MM_CONFIGURE,
                                   HDMI_VDMA_S2MM_START,
                                   HDMI_VDMA_S2MM_STOP,
                                   HDMI_VDMA_S2MM_SELECT,
                                   HDMI_VDMA_S2MM_STATUS);
}

static const struct file_operations hdmi_vdma_fops = {
    .owner = THIS_MODULE,
    .open = hdmi_vdma_open,
    .mmap = hdmi_vdma_mmap,
    .unlocked_ioctl = hdmi_vdma_ioctl,
};

static int hdmi_vdma_probe(struct platform_device* pdev)
{
    struct device_node* mem_np;
    struct hdmi_vdma_dev* m;
    int ret;
    unsigned int i;

    dev_info(&pdev->dev, "hdmi-vdma-client %s probe start\n", HDMI_VDMA_DRIVER_VERSION);

    if (!pdev->dev.of_node)
    {
        dev_err(&pdev->dev, "missing device-tree node\n");
        return -ENODEV;
    }

    m = devm_kzalloc(&pdev->dev, sizeof(*m), GFP_KERNEL);
    if (!m)
        return -ENOMEM;

    m->dev = &pdev->dev;
    m->total_size = HDMI_VDMA_FRAME_MAP_SIZE * HDMI_VDMA_FRAME_COUNT;
    m->mm2s.dir = DMA_MEM_TO_DEV;
    m->mm2s.name = "mm2s";
    m->s2mm.dir = DMA_DEV_TO_MEM;
    m->s2mm.name = "s2mm";

    ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
    if (ret)
    {
        dev_err(&pdev->dev, "failed to set 32-bit coherent DMA mask: %d\n", ret);
        return ret;
    }

    mem_np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
    if (!mem_np)
    {
        dev_err(&pdev->dev, "missing memory-region phandle; refusing generic coherent fallback\n");
        return -ENODEV;
    }
    dev_info(&pdev->dev, "memory-region phandle: %s\n", mem_np->full_name);
    of_node_put(mem_np);

    ret = of_reserved_mem_device_init(&pdev->dev);
    if (ret == 0)
    {
        m->reserved_mem_attached = true;
        dev_info(&pdev->dev, "attached reserved-memory DMA pool\n");
    }
    else
    {
        dev_err(&pdev->dev, "failed to attach reserved-memory DMA pool: %d\n", ret);
        return ret;
    }

    dev_info(&pdev->dev, "requesting dma channel 'mm2s'\n");
    m->mm2s.chan = dma_request_chan(&pdev->dev, "mm2s");
    if (IS_ERR(m->mm2s.chan))
    {
        ret = PTR_ERR(m->mm2s.chan);
        dev_err(&pdev->dev, "failed to request mm2s dma channel: %d\n", ret);
        goto err_release_reserved;
    }

    dev_info(&pdev->dev, "requesting dma channel 's2mm'\n");
    m->s2mm.chan = dma_request_chan(&pdev->dev, "s2mm");
    if (IS_ERR(m->s2mm.chan))
    {
        ret = PTR_ERR(m->s2mm.chan);
        dev_err(&pdev->dev, "failed to request s2mm dma channel: %d\n", ret);
        goto err_release_mm2s;
    }

    dev_info(&pdev->dev,
             "allocating %u shared frame(s), %lu mapped bytes each (%u active bytes each)\n",
             HDMI_VDMA_FRAME_COUNT,
             (unsigned long)HDMI_VDMA_FRAME_MAP_SIZE,
             HDMI_VDMA_FRAME_BYTES);
    for (i = 0; i < HDMI_VDMA_FRAME_COUNT; i++)
    {
        m->frames[i] =
            dma_alloc_coherent(&pdev->dev, HDMI_VDMA_FRAME_MAP_SIZE, &m->frames_dma[i], GFP_KERNEL);
        if (!m->frames[i])
        {
            dev_err(&pdev->dev,
                    "dma_alloc_coherent failed for frame %u (%lu bytes)\n",
                    i,
                    (unsigned long)HDMI_VDMA_FRAME_MAP_SIZE);
            ret = -ENOMEM;
            goto err_free_frames;
        }
        dev_info(&pdev->dev, "frame %u dma=%pad virt=%p\n", i, &m->frames_dma[i], m->frames[i]);
    }

    ret = alloc_chrdev_region(&m->devt, 0, 1, HDMI_VDMA_DEVICE_NAME);
    if (ret)
        goto err_free_frames;

    cdev_init(&m->cdev, &hdmi_vdma_fops);
    m->cdev.owner = THIS_MODULE;
    ret = cdev_add(&m->cdev, m->devt, 1);
    if (ret)
        goto err_unregister;

    m->class = class_create(THIS_MODULE, HDMI_VDMA_DEVICE_NAME);
    if (IS_ERR(m->class))
    {
        ret = PTR_ERR(m->class);
        goto err_cdev;
    }

    m->chardev = device_create(m->class, NULL, m->devt, NULL, HDMI_VDMA_DEVICE_NAME);
    if (IS_ERR(m->chardev))
    {
        ret = PTR_ERR(m->chardev);
        goto err_class;
    }

    platform_set_drvdata(pdev, m);
    g_vdma = m;
    dev_info(&pdev->dev,
             "created /dev/%s, %u shared frames, total mapped size=%zu\n",
             HDMI_VDMA_DEVICE_NAME,
             HDMI_VDMA_FRAME_COUNT,
             m->total_size);
    return 0;

err_class:
    class_destroy(m->class);
err_cdev:
    cdev_del(&m->cdev);
err_unregister:
    unregister_chrdev_region(m->devt, 1);
err_free_frames:
    for (i = 0; i < HDMI_VDMA_FRAME_COUNT; i++)
    {
        if (m->frames[i])
            dma_free_coherent(&pdev->dev, HDMI_VDMA_FRAME_MAP_SIZE, m->frames[i], m->frames_dma[i]);
    }
    dma_release_channel(m->s2mm.chan);
err_release_mm2s:
    dma_release_channel(m->mm2s.chan);
err_release_reserved:
    if (m->reserved_mem_attached)
        of_reserved_mem_device_release(&pdev->dev);
    return ret;
}

static int hdmi_vdma_remove(struct platform_device* pdev)
{
    struct hdmi_vdma_dev* m = platform_get_drvdata(pdev);
    unsigned int i;

    if (!m)
        return 0;

    if (g_vdma == m)
        g_vdma = NULL;

    dmaengine_terminate_sync(m->mm2s.chan);
    dmaengine_terminate_sync(m->s2mm.chan);
    device_destroy(m->class, m->devt);
    class_destroy(m->class);
    cdev_del(&m->cdev);
    unregister_chrdev_region(m->devt, 1);
    for (i = 0; i < HDMI_VDMA_FRAME_COUNT; i++)
    {
        if (m->frames[i])
            dma_free_coherent(&pdev->dev, HDMI_VDMA_FRAME_MAP_SIZE, m->frames[i], m->frames_dma[i]);
    }
    dma_release_channel(m->s2mm.chan);
    dma_release_channel(m->mm2s.chan);
    if (m->reserved_mem_attached)
        of_reserved_mem_device_release(&pdev->dev);
    return 0;
}

static const struct of_device_id hdmi_vdma_of_match[] = {{.compatible = "hdmi-demo,vdma-client-v1"},
                                                         {}};
MODULE_DEVICE_TABLE(of, hdmi_vdma_of_match);

static struct platform_driver hdmi_vdma_driver = {
    .probe = hdmi_vdma_probe,
    .remove = hdmi_vdma_remove,
    .driver =
        {
            .name = "hdmi-vdma-client",
            .of_match_table = hdmi_vdma_of_match,
        },
};
module_platform_driver(hdmi_vdma_driver);

MODULE_AUTHOR("HDMI overlay project");
MODULE_DESCRIPTION("AXI VDMA MM2S/S2MM dmaengine client for HDMI passthrough");
MODULE_LICENSE("GPL");
