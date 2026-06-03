#include "video_output.h"

#include <string.h>

#include "xparameters.h"

/**
 * @brief Initialize output-side dynclk, VTC, and DMA composition state.
 * @param output Output helper to initialize.
 * @param dma Shared DMA adapter used for MM2S.
 * @param stride Framebuffer line stride in bytes.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or a HAL error code on failure.
 */
int video_output_init(video_output_t* const output, video_dma_t* const dma, uint32_t stride)
{
    int status;

    if ((output == NULL) || (dma == NULL) || (stride == 0U))
    {
        return XST_INVALID_PARAM;
    }

    memset(output, 0, sizeof(*output));
    output->dma = dma;
    output->stride = stride;

    status = video_dynclk_init(&output->dynclk);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    status = video_vtc_init(&output->vtc, XPAR_V_TC_0_DEVICE_ID);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    return XST_SUCCESS;
}

/**
 * @brief Start HDMI output for one mode and framebuffer.
 * @param output Initialized output helper.
 * @param mode Supported video mode to generate.
 * @param frame_index Framebuffer index to display.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or a HAL error code on failure.
 */
int video_output_start(video_output_t* const output,
                       video_pipeline_mode_t const* const mode,
                       uint32_t frame_index)
{
    int status;

    if ((output == NULL) || (mode == NULL) || (output->dma == NULL))
    {
        return XST_INVALID_PARAM;
    }

    if (output->running)
    {
        (void)video_output_stop(output);
    }

    status = video_dynclk_configure(&output->dynclk, mode->timing.pixel_clock_mhz);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    status = video_vtc_configure_generator(&output->vtc, &mode->timing);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    video_vtc_start_generator(&output->vtc);

    status = video_dma_configure(output->dma,
                                 VIDEO_DMA_CHANNEL_MM2S,
                                 mode->timing.width,
                                 mode->timing.height,
                                 output->stride,
                                 frame_index);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    status = video_dma_start(output->dma, VIDEO_DMA_CHANNEL_MM2S);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    status = video_dma_select_frame(output->dma, VIDEO_DMA_CHANNEL_MM2S, frame_index);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    output->mode = mode;
    output->frame_index = frame_index;
    output->running = 1;

    return XST_SUCCESS;
}

/**
 * @brief Stop HDMI output timing and MM2S transfer.
 * @param output Initialized output helper.
 * @return XST_SUCCESS on success, or XST_INVALID_PARAM for bad input.
 */
int video_output_stop(video_output_t* const output)
{
    if (output == NULL)
    {
        return XST_INVALID_PARAM;
    }

    video_vtc_stop_generator(&output->vtc);
    if (output->dma != NULL)
    {
        (void)video_dma_stop(output->dma, VIDEO_DMA_CHANNEL_MM2S);
    }

    output->running = 0;
    output->mode = NULL;
    return XST_SUCCESS;
}
