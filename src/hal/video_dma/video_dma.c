/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file video_dma.c
/// @brief HDMI VDMA HAL adapter implementation
///

// === Headers files inclusions ==================================================================================== //

#include "video_dma.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "hdmi_vdma.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static int ioctl_noarg(video_dma_t* dma, unsigned long request, const char* name);
static int configure_channel(video_dma_t* dma,
                             unsigned long request,
                             const char* name,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride,
                             uint32_t frame_index);
static int select_channel(video_dma_t* dma,
                          unsigned long request,
                          const char* name,
                          uint32_t frame_index);
static uint32_t channel_status(video_dma_t* dma, unsigned long request);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Issue a no-argument ioctl to the hdmi-vdma device.
 * @param dma Initialized DMA adapter.
 * @param request ioctl request code.
 * @param name Human-readable operation name used in error logs.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on ioctl failure.
 */
static int ioctl_noarg(video_dma_t* const dma, unsigned long request, const char* const name)
{
    if ((dma == NULL) || !dma->is_open)
    {
        return XST_INVALID_PARAM;
    }

    if (ioctl(dma->fd, request) != 0)
    {
        fprintf(stderr, "[video_dma] %s failed: %s\n", name, strerror(errno));
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

/**
 * @brief Configure one VDMA channel through the kernel client.
 * @param dma Initialized DMA adapter.
 * @param request Channel-specific configure ioctl request.
 * @param name Human-readable operation name used in error logs.
 * @param width Active video width in pixels.
 * @param height Active video height in lines.
 * @param stride Framebuffer line stride in bytes.
 * @param frame_index Framebuffer index to use.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on ioctl failure.
 */
static int configure_channel(video_dma_t* const dma,
                             unsigned long request,
                             const char* const name,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride,
                             uint32_t frame_index)
{
    struct hdmi_vdma_config cfg;

    if ((dma == NULL) || !dma->is_open || (width == 0U) || (height == 0U) || (stride == 0U)
        || (frame_index >= dma->frame_count))
    {
        return XST_INVALID_PARAM;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.width = width;
    cfg.height = height;
    cfg.stride = stride;
    cfg.frame_index = frame_index;

    if (ioctl(dma->fd, request, &cfg) != 0)
    {
        fprintf(stderr, "[video_dma] %s failed: %s\n", name, strerror(errno));
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

/**
 * @brief Select the active framebuffer for one VDMA channel.
 * @param dma Initialized DMA adapter.
 * @param request Channel-specific select ioctl request.
 * @param name Human-readable operation name used in error logs.
 * @param frame_index Framebuffer index to select.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on ioctl failure.
 */
static int select_channel(video_dma_t* const dma,
                          unsigned long request,
                          const char* const name,
                          uint32_t frame_index)
{
    uint32_t kernel_frame = frame_index;

    if ((dma == NULL) || !dma->is_open || (frame_index >= dma->frame_count))
    {
        return XST_INVALID_PARAM;
    }

    if (ioctl(dma->fd, request, &kernel_frame) != 0)
    {
        fprintf(stderr, "[video_dma] %s failed: %s\n", name, strerror(errno));
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

/**
 * @brief Read one VDMA channel status through the kernel client.
 * @param dma Initialized DMA adapter.
 * @param request Channel-specific status ioctl request.
 * @return VIDEO_DMA_STATUS_IDLE when stopped, 0 when running, or VIDEO_DMA_STATUS_HALTED on invalid input/failure.
 */
static uint32_t channel_status(video_dma_t* const dma, unsigned long request)
{
    struct hdmi_vdma_channel_status status;

    if ((dma == NULL) || !dma->is_open)
    {
        return VIDEO_DMA_STATUS_HALTED;
    }

    memset(&status, 0, sizeof(status));
    if (ioctl(dma->fd, request, &status) != 0)
    {
        return VIDEO_DMA_STATUS_HALTED;
    }

    return (status.running != 0U) ? 0U : VIDEO_DMA_STATUS_IDLE;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Open /dev/hdmi-vdma and map coherent framebuffer slots.
 * @param dma DMA adapter to initialize.
 * @param frames Output array filled with mapped framebuffer pointers.
 * @param frame_count Number of framebuffers to map.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on open/ioctl/mmap failure.
 */
int video_dma_init(video_dma_t* const dma,
                   uint8_t* frames[VIDEO_DMA_MAX_FRAMES],
                   uint32_t frame_count)
{
    struct hdmi_vdma_info info;
    uint32_t i;

    if ((dma == NULL) || (frames == NULL) || (frame_count == 0U)
        || (frame_count > VIDEO_DMA_MAX_FRAMES))
    {
        return XST_INVALID_PARAM;
    }

    memset(dma, 0, sizeof(*dma));
    dma->fd = -1;

    dma->fd = open("/dev/" HDMI_VDMA_DEVICE_NAME, O_RDWR | O_SYNC);
    if (dma->fd < 0)
    {
        fprintf(stderr,
                "[video_dma] open /dev/%s failed: %s\n",
                HDMI_VDMA_DEVICE_NAME,
                strerror(errno));
        return XST_FAILURE;
    }
    dma->is_open = 1;

    memset(&info, 0, sizeof(info));
    if (ioctl(dma->fd, HDMI_VDMA_GET_INFO, &info) != 0)
    {
        fprintf(stderr, "[video_dma] HDMI_VDMA_GET_INFO failed: %s\n", strerror(errno));
        video_dma_cleanup(dma);
        return XST_FAILURE;
    }

    if ((info.frame_count == 0U) || (info.frame_size == 0U))
    {
        fprintf(stderr, "[video_dma] /dev/%s reported invalid buffers\n", HDMI_VDMA_DEVICE_NAME);
        video_dma_cleanup(dma);
        return XST_FAILURE;
    }

    dma->frame_count = frame_count;
    dma->frame_size = info.frame_size;
    dma->mmap_size = info.frame_size;

    for (i = 0U; i < frame_count; i++)
    {
        uint32_t const kernel_frame = i % info.frame_count;
        off_t const offset = (off_t)((uintptr_t)kernel_frame * dma->frame_size);
        void* const mapped =
            mmap(NULL, dma->frame_size, PROT_READ | PROT_WRITE, MAP_SHARED, dma->fd, offset);

        if (mapped == MAP_FAILED)
        {
            fprintf(stderr,
                    "[video_dma] mmap frame %u failed: %s\n",
                    kernel_frame,
                    strerror(errno));
            video_dma_cleanup(dma);
            return XST_FAILURE;
        }

        frames[i] = (uint8_t*)mapped;
        dma->frames[i] = frames[i];
    }

    return XST_SUCCESS;
}

/**
 * @brief Stop DMA channels, unmap framebuffers, and close the hdmi-vdma device.
 * @param dma DMA adapter to clean up.
 * @return None.
 */
void video_dma_cleanup(video_dma_t* const dma)
{
    uint32_t i;

    if (dma == NULL)
    {
        return;
    }

    if (dma->is_open)
    {
        (void)ioctl(dma->fd, HDMI_VDMA_S2MM_STOP);
        (void)ioctl(dma->fd, HDMI_VDMA_MM2S_STOP);
    }

    if (dma->mmap_size != 0U)
    {
        for (i = 0U; i < dma->frame_count; i++)
        {
            if (dma->frames[i] != NULL)
            {
                (void)munmap(dma->frames[i], dma->mmap_size);
            }
        }
    }

    if (dma->is_open)
    {
        (void)close(dma->fd);
    }

    memset(dma, 0, sizeof(*dma));
    dma->fd = -1;
}

/**
 * @brief Configure either MM2S or S2MM for a frame geometry.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to configure.
 * @param width Active video width in pixels.
 * @param height Active video height in lines.
 * @param stride Framebuffer line stride in bytes.
 * @param frame_index Framebuffer index to use.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on ioctl failure.
 */
int video_dma_configure(video_dma_t* const dma,
                        video_dma_channel_e channel,
                        uint32_t width,
                        uint32_t height,
                        uint32_t stride,
                        uint32_t frame_index)
{
    if (channel == VIDEO_DMA_CHANNEL_MM2S)
    {
        return configure_channel(dma,
                                 HDMI_VDMA_MM2S_CONFIGURE,
                                 "HDMI_VDMA_MM2S_CONFIGURE",
                                 width,
                                 height,
                                 stride,
                                 frame_index);
    }

    if (channel == VIDEO_DMA_CHANNEL_S2MM)
    {
        return configure_channel(dma,
                                 HDMI_VDMA_S2MM_CONFIGURE,
                                 "HDMI_VDMA_S2MM_CONFIGURE",
                                 width,
                                 height,
                                 stride,
                                 frame_index);
    }

    return XST_INVALID_PARAM;
}

/**
 * @brief Start one DMA channel.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to start.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on ioctl failure.
 */
int video_dma_start(video_dma_t* const dma, video_dma_channel_e channel)
{
    if (channel == VIDEO_DMA_CHANNEL_MM2S)
    {
        return ioctl_noarg(dma, HDMI_VDMA_MM2S_START, "HDMI_VDMA_MM2S_START");
    }

    if (channel == VIDEO_DMA_CHANNEL_S2MM)
    {
        return ioctl_noarg(dma, HDMI_VDMA_S2MM_START, "HDMI_VDMA_S2MM_START");
    }

    return XST_INVALID_PARAM;
}

/**
 * @brief Stop one DMA channel.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to stop.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on ioctl failure.
 */
int video_dma_stop(video_dma_t* const dma, video_dma_channel_e channel)
{
    if (channel == VIDEO_DMA_CHANNEL_MM2S)
    {
        return ioctl_noarg(dma, HDMI_VDMA_MM2S_STOP, "HDMI_VDMA_MM2S_STOP");
    }

    if (channel == VIDEO_DMA_CHANNEL_S2MM)
    {
        return ioctl_noarg(dma, HDMI_VDMA_S2MM_STOP, "HDMI_VDMA_S2MM_STOP");
    }

    return XST_INVALID_PARAM;
}

/**
 * @brief Select the framebuffer used by one DMA channel.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to update.
 * @param frame_index Framebuffer index to select.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on ioctl failure.
 */
int video_dma_select_frame(video_dma_t* const dma,
                           video_dma_channel_e channel,
                           uint32_t frame_index)
{
    if (channel == VIDEO_DMA_CHANNEL_MM2S)
    {
        return select_channel(dma, HDMI_VDMA_MM2S_SELECT, "HDMI_VDMA_MM2S_SELECT", frame_index);
    }

    if (channel == VIDEO_DMA_CHANNEL_S2MM)
    {
        return select_channel(dma, HDMI_VDMA_S2MM_SELECT, "HDMI_VDMA_S2MM_SELECT", frame_index);
    }

    return XST_INVALID_PARAM;
}

/**
 * @brief Read the current status of one DMA channel.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to query.
 * @return VIDEO_DMA_STATUS_IDLE when stopped, 0 when running, or VIDEO_DMA_STATUS_HALTED on invalid input/failure.
 */
uint32_t video_dma_status(video_dma_t* const dma, video_dma_channel_e channel)
{
    if (channel == VIDEO_DMA_CHANNEL_MM2S)
    {
        return channel_status(dma, HDMI_VDMA_MM2S_STATUS);
    }

    if (channel == VIDEO_DMA_CHANNEL_S2MM)
    {
        return channel_status(dma, HDMI_VDMA_S2MM_STATUS);
    }

    return VIDEO_DMA_STATUS_HALTED;
}

// === End of documentation ======================================================================================== //
