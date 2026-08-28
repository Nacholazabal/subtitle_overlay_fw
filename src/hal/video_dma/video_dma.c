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
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hdmi_vdma.h"
#include "log.h"

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

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Issue a no-argument ioctl to the hdmi-vdma device.
 * @param dma Initialized DMA adapter.
 * @param request ioctl request code.
 * @param name Human-readable operation name used in error logs.
 * @return 0 on success, -EINVAL for bad input, or -EIO on ioctl failure.
 */
static int ioctl_noarg(video_dma_t* const dma, unsigned long request, const char* const name)
{
    if ((dma == NULL) || !dma->is_open)
    {
        return -EINVAL;
    }

    if (ioctl(dma->fd, request) != 0)
    {
        LOG_ERROR("video_dma: %s failed: %s", name, strerror(errno));
        return -EIO;
    }

    return 0;
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
 * @return 0 on success, -EINVAL for bad input, or -EIO on ioctl failure.
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
        return -EINVAL;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.width = width;
    cfg.height = height;
    cfg.stride = stride;
    cfg.frame_index = frame_index;

    if (ioctl(dma->fd, request, &cfg) != 0)
    {
        LOG_ERROR("video_dma: %s failed: %s", name, strerror(errno));
        return -EIO;
    }

    return 0;
}

/**
 * @brief Select the active framebuffer for one VDMA channel.
 * @param dma Initialized DMA adapter.
 * @param request Channel-specific select ioctl request.
 * @param name Human-readable operation name used in error logs.
 * @param frame_index Framebuffer index to select.
 * @return 0 on success, -EINVAL for bad input, or -EIO on ioctl failure.
 */
static int select_channel(video_dma_t* const dma,
                          unsigned long request,
                          const char* const name,
                          uint32_t frame_index)
{
    uint32_t kernel_frame = frame_index;

    if ((dma == NULL) || !dma->is_open || (frame_index >= dma->frame_count))
    {
        return -EINVAL;
    }

    if (ioctl(dma->fd, request, &kernel_frame) != 0)
    {
        LOG_ERROR("video_dma: %s failed: %s", name, strerror(errno));
        return -EIO;
    }

    return 0;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Open /dev/hdmi-vdma and claim the requested framebuffer slots.
 * @param dma DMA adapter to initialize.
 * @param frame_count Number of framebuffers to claim.
 * @return 0 on success, -EINVAL for bad input, -ENODEV for device open failure, or -EIO on ioctl/validation failure.
 */
int video_dma_init(video_dma_t* const dma, uint32_t frame_count)
{
    struct hdmi_vdma_info info;

    if ((dma == NULL) || (frame_count == 0U) || (frame_count > VIDEO_DMA_MAX_FRAMES))
    {
        return -EINVAL;
    }

    memset(dma, 0, sizeof(*dma));
    dma->fd = -1;

    dma->fd = open("/dev/" HDMI_VDMA_DEVICE_NAME, O_RDWR | O_SYNC);
    if (dma->fd < 0)
    {
        LOG_ERROR("video_dma: open /dev/%s failed: %s", HDMI_VDMA_DEVICE_NAME, strerror(errno));
        return -ENODEV;
    }
    dma->is_open = 1;

    memset(&info, 0, sizeof(info));
    if (ioctl(dma->fd, HDMI_VDMA_GET_INFO, &info) != 0)
    {
        LOG_ERROR("video_dma: HDMI_VDMA_GET_INFO failed: %s", strerror(errno));
        video_dma_cleanup(dma);
        return -EIO;
    }

    if ((info.frame_count == 0U) || (info.frame_size == 0U))
    {
        LOG_ERROR("video_dma: /dev/%s reported invalid buffers", HDMI_VDMA_DEVICE_NAME);
        video_dma_cleanup(dma);
        return -EIO;
    }

    // Never advertise more frames than the kernel actually exposes. Mapping
    // i % info.frame_count would silently alias framebuffers and leave
    // dma->frame_count claiming buffers a later *_SELECT/config would reject.
    if (info.frame_count < frame_count)
    {
        LOG_ERROR("video_dma: /dev/%s exposes %u frames, %u requested",
                  HDMI_VDMA_DEVICE_NAME,
                  (unsigned)info.frame_count,
                  (unsigned)frame_count);
        video_dma_cleanup(dma);
        return -EINVAL;
    }

    dma->frame_count = frame_count;
    return 0;
}

/**
 * @brief Stop DMA channels and close the hdmi-vdma device.
 * @param dma DMA adapter to clean up.
 * @return None.
 */
void video_dma_cleanup(video_dma_t* const dma)
{
    if (dma == NULL)
    {
        return;
    }

    if (dma->is_open)
    {
        (void)ioctl(dma->fd, HDMI_VDMA_S2MM_STOP);
        (void)ioctl(dma->fd, HDMI_VDMA_MM2S_STOP);
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
 * @return 0 on success, -EINVAL for bad input, or -EIO on ioctl failure.
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

    return -EINVAL;
}

/**
 * @brief Start one DMA channel.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to start.
 * @return 0 on success, -EINVAL for bad input, or -EIO on ioctl failure.
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

    return -EINVAL;
}

/**
 * @brief Stop one DMA channel.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to stop.
 * @return 0 on success, -EINVAL for bad input, or -EIO on ioctl failure.
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

    return -EINVAL;
}

/**
 * @brief Select the framebuffer used by one DMA channel.
 * @param dma Initialized DMA adapter.
 * @param channel DMA channel to update.
 * @param frame_index Framebuffer index to select.
 * @return 0 on success, -EINVAL for bad input, or -EIO on ioctl failure.
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

    return -EINVAL;
}

// === End of documentation ======================================================================================== //
