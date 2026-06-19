/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

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
static uint8_t pixel_is_in_range(int32_t x, int32_t y);
static uint8_t reverse_bits_u8(uint8_t value);
static uint32_t subtitle_bram_pack_word(uint8_t const* src);
static int subtitle_bram_write_full_bitmap(subtitle_bram_t const* bram, uint8_t const* src, uint32_t src_stride);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Validate an initialized subtitle BRAM adapter.
 * @param bram BRAM adapter to validate.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int validate_bram(subtitle_bram_t const* const bram)
{
    if (bram == NULL)
    {
        return -EINVAL;
    }

    if (bram->base == (uintptr_t)0)
    {
        return -APP_ESTATE;
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
static uint8_t pixel_is_in_range(int32_t x, int32_t y)
{
    return ((x >= 0) && (y >= 0) && ((uint32_t)x < SUBTITLE_BRAM_MASK_WIDTH)
            && ((uint32_t)y < SUBTITLE_BRAM_MASK_HEIGHT))
               ? 1U
               : 0U;
}

/**
 * @brief Reverse bit order inside one byte.
 * @param value Input byte.
 * @return Byte with bit 7 moved to bit 0, bit 6 moved to bit 1, and so on.
 */
static uint8_t reverse_bits_u8(uint8_t value)
{
    value = (uint8_t)(((value & 0xF0U) >> 4U) | ((value & 0x0FU) << 4U));
    value = (uint8_t)(((value & 0xCCU) >> 2U) | ((value & 0x33U) << 2U));
    value = (uint8_t)(((value & 0xAAU) >> 1U) | ((value & 0x55U) << 1U));

    return value;
}

/**
 * @brief Pack 32 MSB-first source pixels into one BRAM word.
 * @param src Pointer to four source bitmap bytes.
 * @return BRAM word where bit 0 is the leftmost source pixel.
 */
static uint32_t subtitle_bram_pack_word(uint8_t const* const src)
{
    return ((uint32_t)reverse_bits_u8(src[0]) << 0U) | ((uint32_t)reverse_bits_u8(src[1]) << 8U)
           | ((uint32_t)reverse_bits_u8(src[2]) << 16U) | ((uint32_t)reverse_bits_u8(src[3]) << 24U);
}

/**
 * @brief Fast path for copying a full-size aligned subtitle bitmap.
 * @param bram Initialized BRAM adapter.
 * @param src Source row-major bitmap, MSB-first inside each byte.
 * @param src_stride Source row stride in bytes.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int subtitle_bram_write_full_bitmap(subtitle_bram_t const* const bram,
                                           uint8_t const* const src,
                                           uint32_t src_stride)
{
    volatile uint32_t* const words = bram_words(bram);
    uint32_t row;
    uint32_t word_col;

    for (row = 0U; row < SUBTITLE_BRAM_MASK_HEIGHT; row++)
    {
        uint8_t const* const src_row = &src[(size_t)row * (size_t)src_stride];
        uint32_t const dst_row = row * SUBTITLE_BRAM_WORDS_PER_ROW;

        for (word_col = 0U; word_col < SUBTITLE_BRAM_WORDS_PER_ROW; word_col++)
        {
            words[dst_row + word_col] =
                subtitle_bram_pack_word(&src_row[(size_t)word_col * 4U]);
        }
    }

    return 0;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize the subtitle BRAM adapter from the mapped platform region.
 * @param bram BRAM adapter to initialize.
 * @return 0 on success, or a negative errno-style value on failure.
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
 * @return 0 on success, or a negative errno-style value on failure.
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
 * @return 0 on success or clipped out-of-range pixel, or a negative errno-style value on failure.
 */
int subtitle_bram_set_pixel(subtitle_bram_t* const bram, int32_t x, int32_t y)
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
 * @return 0 on success or clipped out-of-range pixel, or a negative errno-style value on failure.
 */
int subtitle_bram_clear_pixel(subtitle_bram_t* const bram, int32_t x, int32_t y)
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
 * @param src_size Source bitmap size in bytes.
 * @param x Destination x coordinate.
 * @param y Destination y coordinate.
 * @param width Source bitmap width in pixels.
 * @param height Source bitmap height in pixels.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int subtitle_bram_write_bitmap(subtitle_bram_t* const bram,
                               uint8_t const* const src,
                               size_t src_size,
                               int32_t x,
                               int32_t y,
                               uint32_t width,
                               uint32_t height)
{
    volatile uint32_t* words;
    uint32_t row;
    uint32_t col;
    uint32_t src_stride;
    int status = validate_bram(bram);

    if (status != 0)
    {
        return status;
    }

    if ((src == NULL) || (src_size == 0U) || (width == 0U) || (height == 0U))
    {
        return -EINVAL;
    }

    words = bram_words(bram);
    src_stride = (width + 7U) / 8U;
    if ((src_stride == 0U) || (height > (SIZE_MAX / src_stride))
        || (((size_t)height * (size_t)src_stride) > src_size))
    {
        return -EINVAL;
    }

    if ((x == 0) && (y == 0) && (width == SUBTITLE_BRAM_MASK_WIDTH) && (height == SUBTITLE_BRAM_MASK_HEIGHT))
    {
        return subtitle_bram_write_full_bitmap(bram, src, src_stride);
    }

    for (row = 0U; row < height; row++)
    {
        int32_t const dst_y = y + (int32_t)row;

        if ((dst_y < 0) || ((uint32_t)dst_y >= SUBTITLE_BRAM_MASK_HEIGHT))
        {
            continue;
        }

        for (col = 0U; col < width; col++)
        {
            int32_t const dst_x = x + (int32_t)col;
            uint8_t const src_byte = src[(row * src_stride) + (col / 8U)];
            uint32_t const src_bit = (uint32_t)((src_byte >> (7U - (col % 8U))) & 1U);

            if ((dst_x < 0) || ((uint32_t)dst_x >= SUBTITLE_BRAM_MASK_WIDTH))
            {
                continue;
            }

            {
                uint32_t const word_index = ((uint32_t)dst_y * SUBTITLE_BRAM_WORDS_PER_ROW)
                                            + ((uint32_t)dst_x / 32U);
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
