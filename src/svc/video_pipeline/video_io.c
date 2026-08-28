/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file video_io.c
/// @brief Video input and output helper implementations
///

// === Headers files inclusions ==================================================================================== //

#include "video_io.h"

#include <string.h>

#include "log.h"

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
 * @return 0 on success, -EINVAL for bad input, or a HAL error code on failure.
 */
int video_input_init(video_input_t* const input, video_dma_t* const dma, uint32_t stride)
{
    int status;

    if ((input == NULL) || (dma == NULL) || (stride == 0U))
    {
        return -EINVAL;
    }

    memset(input, 0, sizeof(*input));
    input->dma = dma;
    input->stride = stride;

    status = video_gpio_init(&input->gpio);
    if (status != 0)
    {
        return status;
    }

    status = video_vtc_init_detector(&input->vtc);
    if (status != 0)
    {
        return status;
    }

    return 0;
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
 * @return 0 on success, -EINVAL for bad input, or a HAL error code on failure.
 */
int video_input_start_detector(video_input_t* const input, uint32_t now_ms)
{
    int status;

    if (input == NULL)
    {
        return -EINVAL;
    }

    if (input->detector_started)
    {
        return 0;
    }

    status = video_vtc_start_detector(&input->vtc);
    if (status == 0)
    {
        input->detector_started = 1U;
        input->detector_started_ms = now_ms;
    }

    return status;
}

/**
 * @brief Milliseconds elapsed since the timing detector was started.
 * @param input Initialized input helper.
 * @param now_ms Current monotonic time in milliseconds.
 * @return Elapsed milliseconds, or 0 when the detector is not running.
 */
uint32_t video_input_detector_elapsed_ms(video_input_t const* const input, uint32_t const now_ms)
{
    if ((input == NULL) || (input->detector_started == 0U))
    {
        return 0U;
    }

    // Unsigned subtraction wraps consistently, so this stays correct across the
    // uint32 millisecond rollover.
    return now_ms - input->detector_started_ms;
}

/**
 * @brief Clear the timing detector so it can be restarted.
 * @param input Initialized input helper.
 * @return None.
 */
void video_input_reset_detector(video_input_t* const input)
{
    if (input == NULL)
    {
        return;
    }

    input->detector_started = 0U;
    input->detector_started_ms = 0U;
}

/**
 * @brief Read detected HDMI input timing.
 * @param input Initialized input helper.
 * @param timing Output active width and height.
 * @return 0 on valid timing, XST_NO_DATA when timing is not ready, or an error code on bad input/failure.
 */
int video_input_read_timing(video_input_t* const input, video_vtc_timing_t* const timing)
{
    if ((input == NULL) || (timing == NULL))
    {
        return -EINVAL;
    }

    return video_vtc_read_detector_timing(&input->vtc, timing);
}

/**
 * @brief Start S2MM capture into the selected framebuffer.
 * @param input Initialized input helper.
 * @param mode Supported mode that matches the detected input timing.
 * @param frame_index Framebuffer index to capture into.
 * @return 0 on success, -EINVAL for bad input, or a HAL error code on failure.
 */
int video_input_start_capture(video_input_t* const input,
                              video_pipeline_mode_t const* const mode,
                              uint32_t frame_index)
{
    int status;

    if ((input == NULL) || (mode == NULL) || (input->dma == NULL))
    {
        return -EINVAL;
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
    if (status != 0)
    {
        return status;
    }

    status = video_dma_start(input->dma, VIDEO_DMA_CHANNEL_S2MM);
    if (status != 0)
    {
        return status;
    }

    input->running = 1U;

    return 0;
}

/**
 * @brief Stop S2MM capture and reset input detection state.
 * @param input Initialized input helper.
 * @return 0 on success, or -EINVAL for bad input.
 */
int video_input_stop(video_input_t* const input)
{
    if (input == NULL)
    {
        return -EINVAL;
    }

    if (input->dma != NULL)
    {
        (void)video_dma_stop(input->dma, VIDEO_DMA_CHANNEL_S2MM);
    }

    input->running = 0U;
    input->detector_started = 0U;

    return 0;
}

/**
 * @brief Initialize output-side dynclk, VTC, and DMA composition state.
 * @param output Output helper to initialize.
 * @param dma Shared DMA adapter used for MM2S.
 * @param stride Framebuffer line stride in bytes.
 * @return 0 on success, -EINVAL for bad input, or a HAL error code on failure.
 */
int video_output_init(video_output_t* const output, video_dma_t* const dma, uint32_t stride)
{
    int status;

    if ((output == NULL) || (dma == NULL) || (stride == 0U))
    {
        return -EINVAL;
    }

    memset(output, 0, sizeof(*output));
    output->dma = dma;
    output->stride = stride;

    status = video_dynclk_init(&output->dynclk);
    if (status != 0)
    {
        return status;
    }

    status = video_vtc_init_generator(&output->vtc);
    if (status != 0)
    {
        return status;
    }

    return 0;
}

/**
 * @brief Start HDMI output for one mode and framebuffer.
 * @param output Initialized output helper.
 * @param mode Supported video mode to generate.
 * @param frame_index Framebuffer index to display.
 * @return 0 on success, -EINVAL for bad input, or a HAL error code on failure.
 */
int video_output_start(video_output_t* const output,
                       video_pipeline_mode_t const* const mode,
                       uint32_t frame_index)
{
    int status;

    if ((output == NULL) || (mode == NULL) || (output->dma == NULL))
    {
        return -EINVAL;
    }

    if (output->running)
    {
        (void)video_output_stop(output);
    }

    status = video_dynclk_configure(&output->dynclk, mode->timing.pixel_clock_mhz);
    if (status != 0)
    {
        return status;
    }

    // Report the clock the MMCM actually synthesised next to the one the mode
    // table asked for: the difference is the pixel clock error.
    LOG_INFO("video: %s pixel clock requested=%.3f MHz synthesised=%.3f MHz",
             mode->label,
             mode->timing.pixel_clock_mhz,
             output->dynclk.actual_frequency_mhz);

    status = video_vtc_configure_generator(&output->vtc, &mode->timing);
    if (status != 0)
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
    if (status != 0)
    {
        return status;
    }

    status = video_dma_start(output->dma, VIDEO_DMA_CHANNEL_MM2S);
    if (status != 0)
    {
        return status;
    }

    status = video_dma_select_frame(output->dma, VIDEO_DMA_CHANNEL_MM2S, frame_index);
    if (status != 0)
    {
        return status;
    }

    output->mode = mode;
    output->running = 1U;

    return 0;
}

/**
 * @brief Stop HDMI output timing and MM2S transfer.
 * @param output Initialized output helper.
 * @return 0 on success, or -EINVAL for bad input.
 */
int video_output_stop(video_output_t* const output)
{
    if (output == NULL)
    {
        return -EINVAL;
    }

    video_vtc_stop_generator(&output->vtc);
    if (output->dma != NULL)
    {
        (void)video_dma_stop(output->dma, VIDEO_DMA_CHANNEL_MM2S);
    }
    (void)video_dynclk_stop(&output->dynclk);

    output->running = 0U;
    output->mode = NULL;
    return 0;
}

// === End of documentation ======================================================================================== //
