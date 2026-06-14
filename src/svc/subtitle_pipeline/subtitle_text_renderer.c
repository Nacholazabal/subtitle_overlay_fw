/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file subtitle_text_renderer.c
/// @brief Minimal text-to-bitmap renderer for subtitle masks
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_text_renderer.h"

#include <ctype.h>
#include <string.h>

#include "errorno.h"
#include "subtitle_bram.h"

// === Macros definitions ========================================================================================== //

#define GLYPH_WIDTH             (5U)
#define GLYPH_HEIGHT            (7U)
#define GLYPH_SCALE             (4U)
#define GLYPH_RENDERED_WIDTH    (GLYPH_WIDTH * GLYPH_SCALE)
#define GLYPH_RENDERED_HEIGHT   (GLYPH_HEIGHT * GLYPH_SCALE)
#define GLYPH_ADVANCE           (24U)
#define RENDER_LINE_HEIGHT      (32U)
#define RENDER_MAX_LINES        (2U)
#define RENDER_TEXT_X           (8U)
#define RENDER_TEXT_Y           (2U)
#define RENDER_BITMAP_STRIDE    (SUBTITLE_BRAM_MASK_WIDTH / 8U)
#define RENDER_BITMAP_SIZE      (RENDER_BITMAP_STRIDE * SUBTITLE_BRAM_MASK_HEIGHT)
#define RENDER_UNKNOWN_GLYPH    (36U)
#define RENDER_SPACE_GLYPH      (37U)
#define RENDER_PERIOD_GLYPH     (38U)
#define RENDER_COMMA_GLYPH      (39U)
#define RENDER_DASH_GLYPH       (40U)
#define RENDER_APOSTROPHE_GLYPH (41U)
#define RENDER_COLON_GLYPH      (42U)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static uint8_t const* glyph_for_char(char ch);
static void set_bitmap_pixel(uint8_t* dst, uint32_t x, uint32_t y);
static void draw_glyph(uint8_t* dst, uint32_t x, uint32_t y, uint8_t const* glyph);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

static uint8_t const glyphs[][GLYPH_HEIGHT] = {
    {0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU}, // 0
    {0x04U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU}, // 1
    {0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x08U, 0x1FU}, // 2
    {0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU}, // 3
    {0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U}, // 4
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x01U, 0x01U, 0x1EU}, // 5
    {0x0EU, 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x0EU}, // 6
    {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U}, // 7
    {0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU}, // 8
    {0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x01U, 0x0EU}, // 9
    {0x0EU, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U}, // a
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x11U, 0x11U, 0x1EU}, // b
    {0x0EU, 0x11U, 0x10U, 0x10U, 0x10U, 0x11U, 0x0EU}, // c
    {0x1EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x1EU}, // d
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU}, // e
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x10U}, // f
    {0x0EU, 0x11U, 0x10U, 0x17U, 0x11U, 0x11U, 0x0EU}, // g
    {0x11U, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U}, // h
    {0x0EU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU}, // i
    {0x07U, 0x02U, 0x02U, 0x02U, 0x12U, 0x12U, 0x0CU}, // j
    {0x11U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U, 0x11U}, // k
    {0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU}, // l
    {0x11U, 0x1BU, 0x15U, 0x15U, 0x11U, 0x11U, 0x11U}, // m
    {0x11U, 0x19U, 0x15U, 0x13U, 0x11U, 0x11U, 0x11U}, // n
    {0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU}, // o
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U, 0x10U}, // p
    {0x0EU, 0x11U, 0x11U, 0x11U, 0x15U, 0x12U, 0x0DU}, // q
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U}, // r
    {0x0FU, 0x10U, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU}, // s
    {0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U}, // t
    {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU}, // u
    {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x04U}, // v
    {0x11U, 0x11U, 0x11U, 0x15U, 0x15U, 0x15U, 0x0AU}, // w
    {0x11U, 0x11U, 0x0AU, 0x04U, 0x0AU, 0x11U, 0x11U}, // x
    {0x11U, 0x11U, 0x0AU, 0x04U, 0x04U, 0x04U, 0x04U}, // y
    {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x1FU}, // z
    {0x0EU, 0x11U, 0x01U, 0x06U, 0x04U, 0x00U, 0x04U}, // ?
    {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}, // space
    {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x0CU, 0x0CU}, // .
    {0x00U, 0x00U, 0x00U, 0x00U, 0x0CU, 0x04U, 0x08U}, // ,
    {0x00U, 0x00U, 0x00U, 0x1FU, 0x00U, 0x00U, 0x00U}, // -
    {0x04U, 0x04U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U}, // '
    {0x00U, 0x0CU, 0x0CU, 0x00U, 0x0CU, 0x0CU, 0x00U}, // :
};

// === Private function implementation ============================================================================= //

/**
 * @brief Return the 5x7 glyph bitmap for one supported character.
 * @param ch Character to render.
 * @return Glyph row bitmap.
 */
static uint8_t const* glyph_for_char(char ch)
{
    unsigned char const uch = (unsigned char)ch;

    if (isdigit(uch))
    {
        return glyphs[uch - (unsigned char)'0'];
    }

    if (isupper(uch))
    {
        ch = (char)tolower(uch);
    }

    if ((ch >= 'a') && (ch <= 'z'))
    {
        return glyphs[10U + ((uint32_t)ch - (uint32_t)'a')];
    }

    switch (ch)
    {
    case ' ':
        return glyphs[RENDER_SPACE_GLYPH];

    case '.':
        return glyphs[RENDER_PERIOD_GLYPH];

    case ',':
        return glyphs[RENDER_COMMA_GLYPH];

    case '-':
        return glyphs[RENDER_DASH_GLYPH];

    case '\'':
        return glyphs[RENDER_APOSTROPHE_GLYPH];

    case ':':
        return glyphs[RENDER_COLON_GLYPH];

    default:
        return glyphs[RENDER_UNKNOWN_GLYPH];
    }
}

/**
 * @brief Set one pixel in the packed MSB-first bitmap.
 * @param dst Destination bitmap.
 * @param x Pixel x coordinate.
 * @param y Pixel y coordinate.
 * @return None.
 */
static void set_bitmap_pixel(uint8_t* const dst, uint32_t x, uint32_t y)
{
    uint32_t const byte_index = (y * RENDER_BITMAP_STRIDE) + (x / 8U);
    uint8_t const bit_mask = (uint8_t)(1U << (7U - (x % 8U)));

    dst[byte_index] |= bit_mask;
}

/**
 * @brief Draw one glyph into the packed destination bitmap.
 * @param dst Destination bitmap.
 * @param x Glyph x coordinate.
 * @param y Glyph y coordinate.
 * @param glyph Glyph row bitmap.
 * @return None.
 */
static void draw_glyph(uint8_t* const dst, uint32_t x, uint32_t y, uint8_t const* const glyph)
{
    uint32_t row;

    for (row = 0U; row < GLYPH_HEIGHT; row++)
    {
        uint32_t col;
        for (col = 0U; col < GLYPH_WIDTH; col++)
        {
            if ((glyph[row] & (1U << (GLYPH_WIDTH - 1U - col))) != 0U)
            {
                uint32_t scaled_row;
                for (scaled_row = 0U; scaled_row < GLYPH_SCALE; scaled_row++)
                {
                    uint32_t scaled_col;
                    for (scaled_col = 0U; scaled_col < GLYPH_SCALE; scaled_col++)
                    {
                        set_bitmap_pixel(dst,
                                         x + (col * GLYPH_SCALE) + scaled_col,
                                         y + (row * GLYPH_SCALE) + scaled_row);
                    }
                }
            }
        }
    }
}

// === Public function implementation ============================================================================== //

/**
 * @brief Render text into a 256x64 packed subtitle mask bitmap.
 * @param text Null-terminated text to render.
 * @param dst Destination bitmap buffer.
 * @param dst_size Destination buffer size in bytes.
 * @param width Rendered bitmap width destination.
 * @param height Rendered bitmap height destination.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int subtitle_text_renderer_render(char const* const text,
                                  uint8_t* const dst,
                                  size_t dst_size,
                                  uint32_t* const width,
                                  uint32_t* const height)
{
    uint32_t x = RENDER_TEXT_X;
    uint32_t y = RENDER_TEXT_Y;
    uint32_t line = 0U;
    char const* cursor;

    if ((text == NULL) || (dst == NULL) || (width == NULL) || (height == NULL)
        || (dst_size < RENDER_BITMAP_SIZE))
    {
        return -EINVAL;
    }

    memset(dst, 0, dst_size);

    for (cursor = text; (*cursor != '\0') && (line < RENDER_MAX_LINES); cursor++)
    {
        if (*cursor == '\n')
        {
            line++;
            x = RENDER_TEXT_X;
            y += RENDER_LINE_HEIGHT;
            continue;
        }

        if ((x + GLYPH_RENDERED_WIDTH) >= SUBTITLE_BRAM_MASK_WIDTH)
        {
            line++;
            x = RENDER_TEXT_X;
            y += RENDER_LINE_HEIGHT;
            if (line >= RENDER_MAX_LINES)
            {
                break;
            }

            if (*cursor == ' ')
            {
                continue;
            }
        }

        draw_glyph(dst, x, y, glyph_for_char(*cursor));
        x += GLYPH_ADVANCE;
    }

    *width = SUBTITLE_BRAM_MASK_WIDTH;
    *height = SUBTITLE_BRAM_MASK_HEIGHT;
    return 0;
}

// === End of documentation ======================================================================================== //
