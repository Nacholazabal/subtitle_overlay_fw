/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file subtitle_bram.c
/// @brief Subtitle mask BRAM HAL adapter implementation
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_bram.h"

#include <string.h>

#include "errorno.h"
#include "hw_platform.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static int validate_bram(subtitle_bram_t const* bram);
static volatile uint32_t* bram_words(subtitle_bram_t const* bram);
static int pixel_is_in_range(int x, int y);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Validate an initialized subtitle BRAM adapter.
 * @param bram BRAM adapter to validate.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int validate_bram(subtitle_bram_t const* const bram)
{
    if (bram == NULL)
    {
        return -EINVAL;
    }

    if (bram->base == (uintptr_t)0)
    {
        return -ESTATE;
    }

    return 0;
}

/**
 * @brief Return the subtitle BRAM as a volatile 32-bit word array.
 * @param bram Initialized BRAM adapter.
 * @return Volatile word pointer.
 */
static volatile uint32_t* bram_words(subtitle_bram_t const* const bram)
{
    return (volatile uint32_t*)bram->base;
}

/**
 * @brief Check whether a mask pixel coordinate is inside the subtitle BRAM geometry.
 * @param x Pixel x coordinate.
 * @param y Pixel y coordinate.
 * @return Nonzero when in range, zero otherwise.
 */
static int pixel_is_in_range(int x, int y)
{
    return ((x >= 0) && (y >= 0) &&
            ((uint32_t)x < SUBTITLE_BRAM_MASK_WIDTH) &&
            ((uint32_t)y < SUBTITLE_BRAM_MASK_HEIGHT));
}

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize the subtitle BRAM adapter from the mapped platform region.
 * @param bram BRAM adapter to initialize.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_bram_init(subtitle_bram_t* const bram)
{
    if (bram == NULL)
    {
        return -EINVAL;
    }

    memset(bram, 0, sizeof(*bram));
    bram->base = hw_platform_base(HW_REGION_SUBTITLE_BRAM);

    return (bram->base != (uintptr_t)0) ? 0 : -EIO;
}

/**
 * @brief Clear the whole subtitle mask BRAM.
 * @param bram Initialized BRAM adapter.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_bram_clear(subtitle_bram_t* const bram)
{
    volatile uint32_t* words;
    uint32_t i;
    int status = validate_bram(bram);

    if (status != 0)
    {
        return status;
    }

    words = bram_words(bram);
    for (i = 0U; i < SUBTITLE_BRAM_WORD_COUNT; i++)
    {
        words[i] = 0U;
    }

    return 0;
}

/**
 * @brief Set one subtitle mask pixel.
 * @param bram Initialized BRAM adapter.
 * @param x Pixel x coordinate.
 * @param y Pixel y coordinate.
 * @return 0 on success or clipped out-of-range pixel, or a negative errorno_e value on failure.
 */
int subtitle_bram_set_pixel(subtitle_bram_t* const bram, int x, int y)
{
    volatile uint32_t* words;
    uint32_t word_index;
    uint32_t bit_index;
    int status = validate_bram(bram);

    if (status != 0)
    {
        return status;
    }

    if (!pixel_is_in_range(x, y))
    {
        return 0;
    }

    words = bram_words(bram);
    word_index = ((uint32_t)y * SUBTITLE_BRAM_WORDS_PER_ROW) + ((uint32_t)x / 32U);
    bit_index = (uint32_t)x % 32U;
    words[word_index] |= (1U << bit_index);

    return 0;
}

/**
 * @brief Clear one subtitle mask pixel.
 * @param bram Initialized BRAM adapter.
 * @param x Pixel x coordinate.
 * @param y Pixel y coordinate.
 * @return 0 on success or clipped out-of-range pixel, or a negative errorno_e value on failure.
 */
int subtitle_bram_clear_pixel(subtitle_bram_t* const bram, int x, int y)
{
    volatile uint32_t* words;
    uint32_t word_index;
    uint32_t bit_index;
    int status = validate_bram(bram);

    if (status != 0)
    {
        return status;
    }

    if (!pixel_is_in_range(x, y))
    {
        return 0;
    }

    words = bram_words(bram);
    word_index = ((uint32_t)y * SUBTITLE_BRAM_WORDS_PER_ROW) + ((uint32_t)x / 32U);
    bit_index = (uint32_t)x % 32U;
    words[word_index] &= ~(1U << bit_index);

    return 0;
}

/**
 * @brief Copy a packed MSB-first 1bpp bitmap into the subtitle mask BRAM.
 * @param bram Initialized BRAM adapter.
 * @param src Source row-major bitmap, MSB-first inside each byte.
 * @param x Destination x coordinate.
 * @param y Destination y coordinate.
 * @param width Source bitmap width in pixels.
 * @param height Source bitmap height in pixels.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_bram_write_bitmap(subtitle_bram_t* const bram,
                               uint8_t const* const src,
                               int x,
                               int y,
                               int width,
                               int height)
{
    volatile uint32_t* words;
    int row;
    int col;
    int src_stride;
    int status = validate_bram(bram);

    if (status != 0)
    {
        return status;
    }

    if ((src == NULL) || (width <= 0) || (height <= 0))
    {
        return -EINVAL;
    }

    words = bram_words(bram);
    src_stride = (width + 7) / 8;

    for (row = 0; row < height; row++)
    {
        int const dst_y = y + row;

        if ((dst_y < 0) || (dst_y >= (int)SUBTITLE_BRAM_MASK_HEIGHT))
        {
            continue;
        }

        for (col = 0; col < width; col++)
        {
            int const dst_x = x + col;
            uint8_t const src_byte = src[(row * src_stride) + (col / 8)];
            uint32_t const src_bit = (uint32_t)((src_byte >> (7 - (col % 8))) & 1U);

            if ((dst_x < 0) || (dst_x >= (int)SUBTITLE_BRAM_MASK_WIDTH))
            {
                continue;
            }

            {
                uint32_t const word_index =
                    ((uint32_t)dst_y * SUBTITLE_BRAM_WORDS_PER_ROW) + ((uint32_t)dst_x / 32U);
                uint32_t const bit_mask = 1U << ((uint32_t)dst_x % 32U);

                if (src_bit != 0U)
                {
                    words[word_index] |= bit_mask;
                }
                else
                {
                    words[word_index] &= ~bit_mask;
                }
            }
        }
    }

    return 0;
}

// === End of documentation ======================================================================================== //
