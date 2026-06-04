/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file video_io.c
/// @brief Video input and output helper implementations
///

// === Headers files inclusions ==================================================================================== //

#include "video_io.h"

#include <string.h>

#include "xparameters.h"
#include "xstatus.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

/**
 * @brief Initialize input-side GPIO, VTC, and DMA composition state.
 * @param input Input helper to initialize.
 * @param dma Shared DMA adapter used for S2MM.
 * @param stride Framebuffer line stride in bytes.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or a HAL error code on failure.
 */
int video_input_init(video_input_t* const input, video_dma_t* const dma, uint32_t stride)
{
    int status;

    if ((input == NULL) || (dma == NULL) || (stride == 0U))
    {
        return XST_INVALID_PARAM;
    }

    memset(input, 0, sizeof(*input));
    input->dma = dma;
    input->stride = stride;

    status = video_gpio_init(&input->gpio);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    status = video_vtc_init(&input->vtc, XPAR_V_TC_1_DEVICE_ID);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    return XST_SUCCESS;
}

/**
 * @brief Query the HDMI input lock signal.
 * @param input Initialized input helper.
 * @return Nonzero when locked, zero otherwise.
 */
uint8_t video_input_locked(video_input_t const* const input)
{
    if (input == NULL)
    {
        return 0U;
    }

    return video_gpio_is_locked(&input->gpio);
}

/**
 * @brief Start input timing detection if it has not already started.
 * @param input Initialized input helper.
 * @param now_ms Current monotonic time in milliseconds, stored for future timeout policy.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or a HAL error code on failure.
 */
int video_input_start_detector(video_input_t* const input, uint32_t now_ms)
{
    int status;

    if (input == NULL)
    {
        return XST_INVALID_PARAM;
    }

    if (input->detector_started)
    {
        return XST_SUCCESS;
    }

    status = video_vtc_start_detector(&input->vtc);
    if (status == XST_SUCCESS)
    {
        input->detector_started = 1U;
        input->detector_started_ms = now_ms;
    }

    return status;
}

/**
 * @brief Read detected HDMI input timing.
 * @param input Initialized input helper.
 * @param timing Output active width and height.
 * @return XST_SUCCESS on valid timing, XST_NO_DATA when timing is not ready, or an error code on bad input/failure.
 */
int video_input_read_timing(video_input_t* const input, video_vtc_timing_t* const timing)
{
    int status;

    if ((input == NULL) || (timing == NULL))
    {
        return XST_INVALID_PARAM;
    }

    status = video_vtc_read_detector_timing(&input->vtc, timing);
    if (status == XST_SUCCESS)
    {
        input->timing = *timing;
    }

    return status;
}

/**
 * @brief Start S2MM capture into the selected framebuffer.
 * @param input Initialized input helper.
 * @param mode Supported mode that matches the detected input timing.
 * @param frame_index Framebuffer index to capture into.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or a HAL error code on failure.
 */
int video_input_start_capture(video_input_t* const input,
                              video_pipeline_mode_t const* const mode,
                              uint32_t frame_index)
{
    int status;

    if ((input == NULL) || (mode == NULL) || (input->dma == NULL))
    {
        return XST_INVALID_PARAM;
    }

    if (input->running)
    {
        (void)video_input_stop(input);
    }

    status = video_dma_configure(input->dma,
                                 VIDEO_DMA_CHANNEL_S2MM,
                                 mode->timing.width,
                                 mode->timing.height,
                                 input->stride,
                                 frame_index);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    status = video_dma_start(input->dma, VIDEO_DMA_CHANNEL_S2MM);
    if (status != XST_SUCCESS)
    {
        return status;
    }

    input->frame_index = frame_index;
    input->running = 1U;

    return XST_SUCCESS;
}

/**
 * @brief Stop S2MM capture and reset input detection state.
 * @param input Initialized input helper.
 * @return XST_SUCCESS on success, or XST_INVALID_PARAM for bad input.
 */
int video_input_stop(video_input_t* const input)
{
    if (input == NULL)
    {
        return XST_INVALID_PARAM;
    }

    if (input->dma != NULL)
    {
        (void)video_dma_stop(input->dma, VIDEO_DMA_CHANNEL_S2MM);
    }

    input->running = 0U;
    input->detector_started = 0U;
    memset(&input->timing, 0, sizeof(input->timing));

    return XST_SUCCESS;
}

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
    output->running = 1U;

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

    output->running = 0U;
    output->mode = NULL;
    return XST_SUCCESS;
}

// === End of documentation ======================================================================================== //
