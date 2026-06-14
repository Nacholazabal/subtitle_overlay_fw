/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file subtitle_pipeline.c
/// @brief Subtitle pipeline service implementation
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_pipeline.h"

#include <string.h>

#include "errorno.h"
#include "subtitle_text_renderer.h"

// === Macros definitions ========================================================================================== //

#define SUBTITLE_PIPELINE_SOF_TIMEOUT_READS 5000000U

#define SUBTITLE_PIPELINE_BAR_WIDTH_NUMERATOR     5U
#define SUBTITLE_PIPELINE_BAR_WIDTH_DENOMINATOR   6U
#define SUBTITLE_PIPELINE_BAR_HEIGHT_DENOMINATOR  9U
#define SUBTITLE_PIPELINE_BOTTOM_MARGIN_DIVISOR   20U

#define SUBTITLE_PIPELINE_DEFAULT_BAR_WIDTH(display_width) \
    (((display_width) * SUBTITLE_PIPELINE_BAR_WIDTH_NUMERATOR) / SUBTITLE_PIPELINE_BAR_WIDTH_DENOMINATOR)
#define SUBTITLE_PIPELINE_DEFAULT_BAR_HEIGHT(display_height) \
    ((display_height) / SUBTITLE_PIPELINE_BAR_HEIGHT_DENOMINATOR)
#define SUBTITLE_PIPELINE_DEFAULT_BOTTOM_MARGIN(display_height) \
    ((display_height) / SUBTITLE_PIPELINE_BOTTOM_MARGIN_DIVISOR)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static subtitle_overlay_config_t default_config(uint32_t display_width, uint32_t display_height);
static uint8_t pipeline_is_initialized(subtitle_pipeline_t const* pipeline);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Compute legacy subtitle bar defaults from the active display dimensions.
 * @param display_width Active display width in pixels.
 * @param display_height Active display height in lines.
 * @return Overlay configuration for the default subtitle bar.
 */
static subtitle_overlay_config_t default_config(uint32_t display_width, uint32_t display_height)
{
    subtitle_overlay_config_t config;
    uint32_t bar_width = SUBTITLE_PIPELINE_DEFAULT_BAR_WIDTH(display_width);
    uint32_t bar_height = SUBTITLE_PIPELINE_DEFAULT_BAR_HEIGHT(display_height);
    uint32_t margin = SUBTITLE_PIPELINE_DEFAULT_BOTTOM_MARGIN(display_height);

    if (bar_width < SUBTITLE_BRAM_MASK_WIDTH)
    {
        bar_width = SUBTITLE_BRAM_MASK_WIDTH;
    }

    if (bar_height < SUBTITLE_BRAM_MASK_HEIGHT)
    {
        bar_height = SUBTITLE_BRAM_MASK_HEIGHT;
    }

    memset(&config, 0, sizeof(config));
    config.width = bar_width;
    config.height = bar_height;
    config.x = (display_width > bar_width) ? ((display_width - bar_width) / 2U) : 0U;
    config.y = (display_height > (bar_height + margin)) ? (display_height - bar_height - margin)
                                                        : 0U;
    config.bar_color = SUBTITLE_PIPELINE_DEFAULT_BAR_COLOR;
    config.text_color = SUBTITLE_PIPELINE_DEFAULT_TEXT_COLOR;

    return config;
}

/**
 * @brief Check whether a subtitle pipeline is usable.
 * @param pipeline Pipeline instance to validate.
 * @return Nonzero when initialized, zero otherwise.
 */
static uint8_t pipeline_is_initialized(subtitle_pipeline_t const* const pipeline)
{
    return ((pipeline != NULL) && (pipeline->initialized != 0U)) ? 1U : 0U;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize subtitle overlay and BRAM service state.
 * @param pipeline Pipeline instance to initialize.
 * @param display_width Active display width in pixels.
 * @param display_height Active display height in lines.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_init(subtitle_pipeline_t* const pipeline,
                           uint32_t display_width,
                           uint32_t display_height)
{
    int status;

    if ((pipeline == NULL) || (display_width == 0U) || (display_height == 0U))
    {
        return -EINVAL;
    }

    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->display_width = display_width;
    pipeline->display_height = display_height;
    pipeline->config = default_config(display_width, display_height);

    status = subtitle_overlay_init(&pipeline->overlay);
    if (status != 0)
    {
        return status;
    }

    status = subtitle_bram_init(&pipeline->bram);
    if (status != 0)
    {
        return status;
    }

    status = subtitle_overlay_configure(&pipeline->overlay, &pipeline->config);
    if (status != 0)
    {
        return status;
    }

    status = subtitle_bram_clear(&pipeline->bram);
    if (status != 0)
    {
        return status;
    }

    status = subtitle_overlay_enable(&pipeline->overlay, 0);
    if (status != 0)
    {
        return status;
    }

    pipeline->initialized = 1U;
    pipeline->enabled = 0U;
    return 0;
}

/**
 * @brief Disable subtitle overlay and reset service state.
 * @param pipeline Pipeline instance to clean up.
 * @return None.
 */
void subtitle_pipeline_cleanup(subtitle_pipeline_t* const pipeline)
{
    if (pipeline == NULL)
    {
        return;
    }

    if (pipeline_is_initialized(pipeline))
    {
        (void)subtitle_overlay_enable(&pipeline->overlay, 0);
    }

    memset(pipeline, 0, sizeof(*pipeline));
}

/**
 * @brief Clear the subtitle mask.
 * @param pipeline Initialized pipeline instance.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_clear(subtitle_pipeline_t* const pipeline)
{
    if (!pipeline_is_initialized(pipeline))
    {
        return (pipeline == NULL) ? -EINVAL : -ESTATE;
    }

    return subtitle_bram_clear(&pipeline->bram);
}

/**
 * @brief Write a packed MSB-first bitmap into the subtitle mask.
 * @param pipeline Initialized pipeline instance.
 * @param src Source row-major bitmap.
 * @param src_size Source bitmap size in bytes.
 * @param x Destination x coordinate.
 * @param y Destination y coordinate.
 * @param width Source bitmap width in pixels.
 * @param height Source bitmap height in pixels.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_write_bitmap(subtitle_pipeline_t* const pipeline,
                                   uint8_t const* const src,
                                   size_t src_size,
                                   int32_t x,
                                   int32_t y,
                                   uint32_t width,
                                   uint32_t height)
{
    if (!pipeline_is_initialized(pipeline))
    {
        return (pipeline == NULL) ? -EINVAL : -ESTATE;
    }

    return subtitle_bram_write_bitmap(&pipeline->bram, src, src_size, x, y, width, height);
}

/**
 * @brief Render text into a subtitle mask and write it to BRAM.
 * @param pipeline Initialized pipeline instance.
 * @param text Null-terminated subtitle text.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_write_text(subtitle_pipeline_t* const pipeline, char const* const text)
{
    uint8_t bitmap[SUBTITLE_BRAM_SIZE_BYTES];
    uint32_t width;
    uint32_t height;
    int status;

    if (!pipeline_is_initialized(pipeline))
    {
        return (pipeline == NULL) ? -EINVAL : -ESTATE;
    }

    status = subtitle_text_renderer_render(text, bitmap, sizeof(bitmap), &width, &height);
    if (status != 0)
    {
        return status;
    }

    return subtitle_bram_write_bitmap(&pipeline->bram, bitmap, sizeof(bitmap), 0, 0, width, height);
}

/**
 * @brief Blocking debug/manual API: synchronize subtitle updates to the next overlay SOF boundary.
 *
 * This function can spin through many MMIO reads. Do not call it from QP/C AO
 * state handlers or other cooperative-loop paths.
 * @param pipeline Initialized pipeline instance.
 * @return 0 when SOF is observed, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_commit(subtitle_pipeline_t* const pipeline)
{
    int status;

    if (!pipeline_is_initialized(pipeline))
    {
        return (pipeline == NULL) ? -EINVAL : -ESTATE;
    }

    status = subtitle_overlay_clear_sof(&pipeline->overlay);
    if (status != 0)
    {
        return status;
    }

    status = subtitle_overlay_wait_sof(&pipeline->overlay, SUBTITLE_PIPELINE_SOF_TIMEOUT_READS);
    return status;
}

/**
 * @brief Clear the overlay SOF flag without waiting for a future SOF.
 * @param pipeline Initialized pipeline instance.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_clear_sof(subtitle_pipeline_t* const pipeline)
{
    if (!pipeline_is_initialized(pipeline))
    {
        return (pipeline == NULL) ? -EINVAL : -ESTATE;
    }

    return subtitle_overlay_clear_sof(&pipeline->overlay);
}

/**
 * @brief Poll the overlay SOF flag once without blocking.
 * @param pipeline Initialized pipeline instance.
 * @param sof_seen Output flag set to one when SOF is currently asserted.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_poll_sof(subtitle_pipeline_t* const pipeline, uint8_t* const sof_seen)
{
    uint32_t control;
    int status;

    if (!pipeline_is_initialized(pipeline))
    {
        return (pipeline == NULL) ? -EINVAL : -ESTATE;
    }

    if (sof_seen == NULL)
    {
        return -EINVAL;
    }

    status = subtitle_overlay_read_control(&pipeline->overlay, &control);
    if (status != 0)
    {
        return status;
    }

    *sof_seen = ((control & SUBTITLE_OVERLAY_CTRL_SOF) != 0U) ? 1U : 0U;
    return 0;
}

/**
 * @brief Enable or disable subtitle overlay compositing.
 * @param pipeline Initialized pipeline instance.
 * @param enabled Nonzero enables overlay, zero disables overlay passthrough.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_pipeline_enable(subtitle_pipeline_t* const pipeline, uint8_t enabled)
{
    int status;

    if (!pipeline_is_initialized(pipeline))
    {
        return (pipeline == NULL) ? -EINVAL : -ESTATE;
    }

    status = subtitle_overlay_enable(&pipeline->overlay, enabled);
    if (status == 0)
    {
        pipeline->enabled = (enabled != 0U) ? 1U : 0U;
    }

    return status;
}

// === End of documentation ======================================================================================== //
