/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file video_gpio.c
/// @brief Video GPIO HAL adapter implementation
///

// === Headers files inclusions ==================================================================================== //

#include "video_gpio.h"

#include "hw_platform.h"
#include "xil_io.h"

// === Macros definitions ========================================================================================== //

#define XGPIO_DATA_OFFSET  0x000U
#define XGPIO_TRI_OFFSET   0x004U
#define XGPIO_DATA2_OFFSET 0x008U
#define XGPIO_TRI2_OFFSET  0x00CU

#define HPD_MASK    0x00000001U
#define LOCKED_MASK 0x00000001U

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

/**
 * @brief Initialize the video GPIO and assert HDMI hot-plug detect.
 * @param gpio GPIO adapter to initialize.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE when the region is not mapped.
 */
int video_gpio_init(video_gpio_t* const gpio)
{
    if (gpio == NULL)
    {
        return XST_INVALID_PARAM;
    }

    gpio->base = hw_platform_base(HW_REGION_VIDEO_GPIO);
    if (gpio->base == (uintptr_t)0)
    {
        return XST_FAILURE;
    }

    Xil_Out32(gpio->base + XGPIO_DATA_OFFSET, 0U);
    Xil_Out32(gpio->base + XGPIO_TRI_OFFSET, 0U);
    Xil_Out32(gpio->base + XGPIO_TRI2_OFFSET, LOCKED_MASK);
    Xil_Out32(gpio->base + XGPIO_DATA_OFFSET, HPD_MASK);

    return XST_SUCCESS;
}

/**
 * @brief Drive the HDMI hot-plug detect output.
 * @param gpio Initialized GPIO adapter.
 * @param enabled Nonzero asserts HPD, zero deasserts it.
 * @return None.
 */
void video_gpio_set_hpd(video_gpio_t* const gpio, int enabled)
{
    if ((gpio == NULL) || (gpio->base == (uintptr_t)0))
    {
        return;
    }

    Xil_Out32(gpio->base + XGPIO_DATA_OFFSET, enabled ? HPD_MASK : 0U);
}

/**
 * @brief Read the HDMI input lock signal.
 * @param gpio Initialized GPIO adapter.
 * @return Nonzero when the input clock is locked, zero otherwise.
 */
int video_gpio_is_locked(video_gpio_t const* const gpio)
{
    if ((gpio == NULL) || (gpio->base == (uintptr_t)0))
    {
        return 0;
    }

    return (Xil_In32(gpio->base + XGPIO_DATA2_OFFSET) & LOCKED_MASK) != 0U;
}

// === End of documentation ======================================================================================== //
