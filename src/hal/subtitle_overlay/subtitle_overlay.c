/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file subtitle_overlay.c
/// @brief Subtitle overlay AXI-Lite HAL adapter implementation
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_overlay.h"

#include <string.h>

#include "errorno.h"
#include "hw_platform.h"
#include "xil_io.h"

// === Macros definitions ========================================================================================== //

#define SUBTITLE_OVERLAY_REG_POS        0x00U
#define SUBTITLE_OVERLAY_REG_SIZE       0x04U
#define SUBTITLE_OVERLAY_REG_BAR_COLOR  0x08U
#define SUBTITLE_OVERLAY_REG_TEXT_COLOR 0x0CU
#define SUBTITLE_OVERLAY_REG_CTRL       0x10U

#define SUBTITLE_OVERLAY_PACK_POS(x, y)  ((((uint32_t)(y)) << 16U) | (((uint32_t)(x)) & 0xFFFFU))
#define SUBTITLE_OVERLAY_PACK_SIZE(w, h) ((((uint32_t)(h)) << 16U) | (((uint32_t)(w)) & 0xFFFFU))
#define SUBTITLE_OVERLAY_RGB_MASK        0x00FFFFFFU

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static int validate_overlay(subtitle_overlay_t const* overlay);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Validate an initialized subtitle overlay adapter.
 * @param overlay Overlay adapter to validate.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int validate_overlay(subtitle_overlay_t const* const overlay)
{
    if (overlay == NULL)
    {
        return -EINVAL;
    }

    if (overlay->base == (uintptr_t)0)
    {
        return -ESTATE;
    }

    return 0;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize the subtitle overlay adapter from the mapped platform region.
 * @param overlay Overlay adapter to initialize.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_overlay_init(subtitle_overlay_t* const overlay)
{
    if (overlay == NULL)
    {
        return -EINVAL;
    }

    memset(overlay, 0, sizeof(*overlay));
    overlay->base = hw_platform_base(HW_REGION_OVERLAY);

    return (overlay->base != (uintptr_t)0) ? 0 : -EIO;
}

/**
 * @brief Configure subtitle overlay geometry and colors.
 * @param overlay Initialized overlay adapter.
 * @param config Overlay geometry and RGB888 color values.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_overlay_configure(subtitle_overlay_t* const overlay,
                               subtitle_overlay_config_t const* const config)
{
    int status = validate_overlay(overlay);

    if (status != 0)
    {
        return status;
    }

    if ((config == NULL) || (config->width == 0U) || (config->height == 0U) || (config->x > 0xFFFFU)
        || (config->y > 0xFFFFU) || (config->width > 0xFFFFU) || (config->height > 0xFFFFU)
        || ((config->bar_color & ~SUBTITLE_OVERLAY_RGB_MASK) != 0U)
        || ((config->text_color & ~SUBTITLE_OVERLAY_RGB_MASK) != 0U))
    {
        return -EINVAL;
    }

    Xil_Out32(overlay->base + SUBTITLE_OVERLAY_REG_POS,
              SUBTITLE_OVERLAY_PACK_POS(config->x, config->y));
    Xil_Out32(overlay->base + SUBTITLE_OVERLAY_REG_SIZE,
              SUBTITLE_OVERLAY_PACK_SIZE(config->width, config->height));
    Xil_Out32(overlay->base + SUBTITLE_OVERLAY_REG_BAR_COLOR, config->bar_color);
    Xil_Out32(overlay->base + SUBTITLE_OVERLAY_REG_TEXT_COLOR, config->text_color);

    return 0;
}

/**
 * @brief Enable or disable subtitle overlay compositing without changing other control bits.
 * @param overlay Initialized overlay adapter.
 * @param enabled Nonzero enables overlay, zero disables overlay passthrough.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_overlay_enable(subtitle_overlay_t* const overlay, int enabled)
{
    uint32_t control;
    int status = validate_overlay(overlay);

    if (status != 0)
    {
        return status;
    }

    control = Xil_In32(overlay->base + SUBTITLE_OVERLAY_REG_CTRL);
    if (enabled != 0)
    {
        control |= SUBTITLE_OVERLAY_CTRL_ENABLE;
    }
    else
    {
        control &= ~SUBTITLE_OVERLAY_CTRL_ENABLE;
    }

    Xil_Out32(overlay->base + SUBTITLE_OVERLAY_REG_CTRL, control);
    return 0;
}

/**
 * @brief Read the overlay control register.
 * @param overlay Initialized overlay adapter.
 * @param control Output control register value.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_overlay_read_control(subtitle_overlay_t const* const overlay, uint32_t* const control)
{
    int status = validate_overlay(overlay);

    if (status != 0)
    {
        return status;
    }

    if (control == NULL)
    {
        return -EINVAL;
    }

    *control = Xil_In32(overlay->base + SUBTITLE_OVERLAY_REG_CTRL);
    return 0;
}

/**
 * @brief Clear the hardware sticky SOF flag while preserving the overlay enable bit.
 * @param overlay Initialized overlay adapter.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_overlay_clear_sof(subtitle_overlay_t* const overlay)
{
    uint32_t control;
    int status = validate_overlay(overlay);

    if (status != 0)
    {
        return status;
    }

    control = Xil_In32(overlay->base + SUBTITLE_OVERLAY_REG_CTRL);
    Xil_Out32(overlay->base + SUBTITLE_OVERLAY_REG_CTRL, control & ~SUBTITLE_OVERLAY_CTRL_SOF);

    return 0;
}

/**
 * @brief Wait for the overlay SOF flag to be set by hardware.
 * @param overlay Initialized overlay adapter.
 * @param timeout_reads Maximum register reads before timing out.
 * @return 0 when SOF is observed, or a negative errorno_e value on failure.
 */
int subtitle_overlay_wait_sof(subtitle_overlay_t* const overlay, uint32_t timeout_reads)
{
    int status = validate_overlay(overlay);

    if (status != 0)
    {
        return status;
    }

    if (timeout_reads == 0U)
    {
        return -EINVAL;
    }

    while (timeout_reads > 0U)
    {
        if ((Xil_In32(overlay->base + SUBTITLE_OVERLAY_REG_CTRL) & SUBTITLE_OVERLAY_CTRL_SOF) != 0U)
        {
            return 0;
        }
        timeout_reads--;
    }

    return -EAGAIN;
}

// === End of documentation ======================================================================================== //
