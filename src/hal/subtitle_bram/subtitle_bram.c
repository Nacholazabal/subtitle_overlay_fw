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
#include "xparameters_linux.h"

// === Macros definitions ========================================================================================== //

// Build-time guard: the subtitle mask must never be larger than the AXI BRAM
// controller's address window, otherwise subtitle_bram_clear() and the renderer
// would write past the mapped BRAM. gnu99-safe negative-array-size assert — it
// fails the compile if the two contracts drift apart. Keep the bound in sync with
// the flashed bitstream, not the reference HW folder (which is stale).
#define SUBTITLE_BRAM_WINDOW_BYTES \
    ((XPAR_AXI_BRAM_CTRL_0_S_AXI_HIGHADDR - XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR) + 1U)
typedef char subtitle_bram_mask_fits_window_assert
    [(SUBTITLE_BRAM_SIZE_BYTES <= SUBTITLE_BRAM_WINDOW_BYTES) ? 1 : -1];
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static int validate_bram(subtitle_bram_t const* bram);
static volatile uint32_t* bram_words(subtitle_bram_t const* bram);
static uint8_t reverse_bits_u8(uint8_t value);
static uint32_t subtitle_bram_pack_word(uint8_t const* src);
static int subtitle_bram_write_full_bitmap(subtitle_bram_t* const bram,
                                           uint8_t const* const src,
                                           uint32_t const src_stride,
                                           uint32_t const width,
                                           uint32_t const height);

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
           | ((uint32_t)reverse_bits_u8(src[2]) << 16U)
           | ((uint32_t)reverse_bits_u8(src[3]) << 24U);
}

/**
 * @brief Fast word-at-a-time path with shadow-diff for word-aligned bitmaps (F2 Stage 1+2).
 *
 * Writes whole 32-bit words per row with no per-pixel MMIO reads (Stage 1).
 * Compares each word against the shadow and only writes changed words (Stage 2).
 * Partial-to-partial updates typically change only a few hundred words out of
 * ~2800, so this is a ~5-10× win on typical updates over blindly writing all words.
 *
 * @param bram Initialized BRAM adapter.
 * @param src Source row-major bitmap, MSB-first inside each byte.
 * @param src_stride Source row stride in bytes.
 * @param width Bitmap width in pixels (must be multiple of 32).
 * @param height Bitmap height in pixels.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int subtitle_bram_write_full_bitmap(subtitle_bram_t* const bram,
                                           uint8_t const* const src,
                                           uint32_t const src_stride,
                                           uint32_t const width,
                                           uint32_t const height)
{
    volatile uint32_t* const words = bram_words(bram);
    uint32_t const words_per_row = width / 32U;
    uint32_t row;
    uint32_t word_col;

    for (row = 0U; row < height; row++)
    {
        uint8_t const* const src_row = &src[(size_t)row * (size_t)src_stride];
        uint32_t const dst_row = row * SUBTITLE_BRAM_WORDS_PER_ROW;

        for (word_col = 0U; word_col < words_per_row; word_col++)
        {
            uint32_t const word_index = dst_row + word_col;
            uint32_t const new_word = subtitle_bram_pack_word(&src_row[(size_t)word_col * 4U]);

            // F2 Stage 2: shadow-diff — only write changed words to MMIO
            if (new_word != bram->shadow[word_index])
            {
                words[word_index] = new_word;
                bram->shadow[word_index] = new_word;
            }
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

    // F2 Stage 2: shadow buffer already zeroed by memset above

    return (bram->base != (uintptr_t)0) ? 0 : -EIO;
}

/**
 * @brief Clear the whole subtitle mask BRAM and its shadow.
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
        bram->shadow[i] = 0U; // F2 Stage 2: keep shadow in sync
    }

    return 0;
}

/**
 * @brief Copy a packed MSB-first 1bpp bitmap into the subtitle mask BRAM.
 * @param bram Initialized BRAM adapter.
 * @param src Source row-major bitmap, MSB-first inside each byte.
 * @param src_stride Source bitmap stride in bytes.
 * @param x Destination x coordinate.
 * @param y Destination y coordinate.
 * @param width Source bitmap width in pixels.
 * @param height Source bitmap height in pixels.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int subtitle_bram_write_bitmap(subtitle_bram_t* const bram,
                               uint8_t const* const src,
                               uint32_t const src_stride,
                               int32_t x,
                               int32_t y,
                               uint32_t width,
                               uint32_t height)
{
    volatile uint32_t* words;
    uint32_t row;
    uint32_t col;
    int status = validate_bram(bram);

    if (status != 0)
    {
        return status;
    }

    if ((src == NULL) || (src_stride == 0U) || (width == 0U) || (height == 0U))
    {
        return -EINVAL;
    }

    // F2 Stage 1: Word-aligned fast path for production geometry.
    // Renderer emits 32-px-aligned width, so x=0 + word-multiple width enables this.
    if ((x == 0) && (y == 0) && ((width % 32U) == 0U)
        && (width <= SUBTITLE_BRAM_MASK_WIDTH) && (height <= SUBTITLE_BRAM_MASK_HEIGHT))
    {
        return subtitle_bram_write_full_bitmap(bram, src, src_stride, width, height);
    }

    // Fallback per-pixel path for unaligned geometry (tests only)
    words = bram_words(bram);

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
            uint8_t const src_byte = src[((size_t)row * (size_t)src_stride) + (col / 8U)];
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
