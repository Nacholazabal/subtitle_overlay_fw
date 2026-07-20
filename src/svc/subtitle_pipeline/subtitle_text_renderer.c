/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file subtitle_text_renderer.c
/// @brief Minimal text-to-bitmap renderer for subtitle masks
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_text_renderer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "errorno.h"
#include "subtitle_bram.h"
#include "subtitle_text_sanitize.h"

// === Macros definitions ========================================================================================== //

#define GLYPH_WIDTH  (8U)
#define GLYPH_HEIGHT (12U)
// Scale 2 keeps the 8x12 subtitle font crisp while preserving three compact rows.
#define GLYPH_SCALE           (2U)
#define GLYPH_RENDERED_WIDTH  (GLYPH_WIDTH * GLYPH_SCALE)
#define GLYPH_RENDERED_HEIGHT (GLYPH_HEIGHT * GLYPH_SCALE)
#define GLYPH_ADVANCE         (9U * GLYPH_SCALE)
#define RENDER_LINE_HEIGHT    (16U * GLYPH_SCALE)
#define RENDER_MAX_LINES      (3U)
#define RENDER_PREVIOUS_LINES (1U)
#define RENDER_CURRENT_LINES  (RENDER_MAX_LINES - RENDER_PREVIOUS_LINES)
#define RENDER_TEXT_X         (24U)
#define RENDER_TEXT_BLOCK_HEIGHT \
    (GLYPH_RENDERED_HEIGHT + ((RENDER_MAX_LINES - 1U) * RENDER_LINE_HEIGHT))
#define RENDER_TEXT_Y                                                    \
    ((SUBTITLE_BRAM_MASK_HEIGHT > RENDER_TEXT_BLOCK_HEIGHT)              \
         ? ((SUBTITLE_BRAM_MASK_HEIGHT - RENDER_TEXT_BLOCK_HEIGHT) / 2U) \
         : 0U)
#define RENDER_BITMAP_STRIDE ((size_t)SUBTITLE_BRAM_MASK_WIDTH / 8U)
#define RENDER_BITMAP_SIZE   (RENDER_BITMAP_STRIDE * SUBTITLE_BRAM_MASK_HEIGHT)
#define RENDER_SANITIZE_MAX  (512U)
#define RENDER_GLYPHS_PER_LINE \
    (((SUBTITLE_BRAM_MASK_WIDTH - RENDER_TEXT_X - GLYPH_RENDERED_WIDTH) / GLYPH_ADVANCE) + 1U)
#define RENDER_LAYOUT_MAX_LINES (32U)
#define RENDER_LINE_TEXT_MAX    ((size_t)RENDER_GLYPHS_PER_LINE)
#define RENDER_UNKNOWN_GLYPH    (36U)
#define RENDER_SPACE_GLYPH      (37U)
#define RENDER_PERIOD_GLYPH     (38U)
#define RENDER_COMMA_GLYPH      (39U)
#define RENDER_DASH_GLYPH       (40U)
#define RENDER_APOSTROPHE_GLYPH (41U)
#define RENDER_COLON_GLYPH      (42U)
#define RENDER_LINE_PREVIOUS    (0U)
#define RENDER_LINE_CURRENT     (1U)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static uint8_t const* glyph_for_char(char ch);
static void set_bitmap_pixel(uint8_t* dst, uint32_t x, uint32_t y);
static void draw_glyph(uint8_t* dst, uint32_t x, uint32_t y, uint8_t const* glyph, uint8_t dimmed);
static void append_layout_line(char lines[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                               uint32_t* line_count,
                               char const* line);
static void append_word(char lines[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                        uint32_t* line_count,
                        char current[RENDER_GLYPHS_PER_LINE + 1U],
                        char const* word,
                        size_t word_len);
static int sanitize_slice(char const* src, size_t len, char* dst, size_t dst_size);
static uint32_t
build_wrapped_lines(char const* text,
                    char lines[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U]);
static uint32_t copy_tail_lines(char dst[RENDER_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                                uint8_t dst_roles[RENDER_MAX_LINES],
                                uint32_t dst_start,
                                uint32_t max_lines,
                                uint8_t role,
                                char src[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                                uint32_t src_count);
static int build_visible_lines(char const* text,
                               char lines[RENDER_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                               uint8_t line_roles[RENDER_MAX_LINES],
                               uint32_t* line_count);
static void draw_text_line(uint8_t* dst, char const* text, uint32_t line, uint8_t dimmed);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

static uint8_t const glyphs[][GLYPH_HEIGHT] = {
    {0x00U, 0x3CU, 0x66U, 0x6EU, 0x76U, 0x66U, 0x66U, 0x66U, 0x66U, 0x3CU, 0x00U, 0x00U}, // 0
    {0x00U, 0x18U, 0x38U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x7EU, 0x00U, 0x00U}, // 1
    {0x00U, 0x3CU, 0x66U, 0x06U, 0x0CU, 0x18U, 0x30U, 0x60U, 0x66U, 0x7EU, 0x00U, 0x00U}, // 2
    {0x00U, 0x3CU, 0x66U, 0x06U, 0x1CU, 0x06U, 0x06U, 0x06U, 0x66U, 0x3CU, 0x00U, 0x00U}, // 3
    {0x00U, 0x0CU, 0x1CU, 0x3CU, 0x6CU, 0xCCU, 0xFEU, 0x0CU, 0x0CU, 0x1EU, 0x00U, 0x00U}, // 4
    {0x00U, 0x7EU, 0x60U, 0x60U, 0x7CU, 0x06U, 0x06U, 0x06U, 0x66U, 0x3CU, 0x00U, 0x00U}, // 5
    {0x00U, 0x1CU, 0x30U, 0x60U, 0x7CU, 0x66U, 0x66U, 0x66U, 0x66U, 0x3CU, 0x00U, 0x00U}, // 6
    {0x00U, 0x7EU, 0x66U, 0x06U, 0x0CU, 0x18U, 0x18U, 0x30U, 0x30U, 0x30U, 0x00U, 0x00U}, // 7
    {0x00U, 0x3CU, 0x66U, 0x66U, 0x3CU, 0x66U, 0x66U, 0x66U, 0x66U, 0x3CU, 0x00U, 0x00U}, // 8
    {0x00U, 0x3CU, 0x66U, 0x66U, 0x66U, 0x3EU, 0x06U, 0x0CU, 0x18U, 0x70U, 0x00U, 0x00U}, // 9
    {0x00U, 0x00U, 0x00U, 0x3CU, 0x06U, 0x3EU, 0x66U, 0x66U, 0x66U, 0x3EU, 0x00U, 0x00U}, // a
    {0x00U, 0x60U, 0x60U, 0x7CU, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x7CU, 0x00U, 0x00U}, // b
    {0x00U, 0x00U, 0x00U, 0x3CU, 0x66U, 0x60U, 0x60U, 0x60U, 0x66U, 0x3CU, 0x00U, 0x00U}, // c
    {0x00U, 0x06U, 0x06U, 0x3EU, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x3EU, 0x00U, 0x00U}, // d
    {0x00U, 0x00U, 0x00U, 0x3CU, 0x66U, 0x7EU, 0x60U, 0x60U, 0x66U, 0x3CU, 0x00U, 0x00U}, // e
    {0x00U, 0x1CU, 0x36U, 0x30U, 0x30U, 0x7CU, 0x30U, 0x30U, 0x30U, 0x78U, 0x00U, 0x00U}, // f
    {0x00U, 0x00U, 0x00U, 0x3EU, 0x66U, 0x66U, 0x66U, 0x3EU, 0x06U, 0x66U, 0x3CU, 0x00U}, // g
    {0x00U, 0x60U, 0x60U, 0x7CU, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x00U, 0x00U}, // h
    {0x00U, 0x18U, 0x00U, 0x38U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x3CU, 0x00U, 0x00U}, // i
    {0x00U, 0x0CU, 0x00U, 0x1CU, 0x0CU, 0x0CU, 0x0CU, 0x0CU, 0x0CU, 0xCCU, 0x78U, 0x00U}, // j
    {0x00U, 0x60U, 0x60U, 0x66U, 0x6CU, 0x78U, 0x70U, 0x78U, 0x6CU, 0x66U, 0x00U, 0x00U}, // k
    {0x00U, 0x38U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x18U, 0x3CU, 0x00U, 0x00U}, // l
    {0x00U, 0x00U, 0x00U, 0xECU, 0xFEU, 0xD6U, 0xD6U, 0xC6U, 0xC6U, 0xC6U, 0x00U, 0x00U}, // m
    {0x00U, 0x00U, 0x00U, 0x7CU, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x00U, 0x00U}, // n
    {0x00U, 0x00U, 0x00U, 0x3CU, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x3CU, 0x00U, 0x00U}, // o
    {0x00U, 0x00U, 0x00U, 0x7CU, 0x66U, 0x66U, 0x66U, 0x7CU, 0x60U, 0x60U, 0xF0U, 0x00U}, // p
    {0x00U, 0x00U, 0x00U, 0x3EU, 0x66U, 0x66U, 0x66U, 0x3EU, 0x06U, 0x06U, 0x0FU, 0x00U}, // q
    {0x00U, 0x00U, 0x00U, 0x6CU, 0x76U, 0x60U, 0x60U, 0x60U, 0x60U, 0xF0U, 0x00U, 0x00U}, // r
    {0x00U, 0x00U, 0x00U, 0x3EU, 0x60U, 0x60U, 0x3CU, 0x06U, 0x06U, 0x7CU, 0x00U, 0x00U}, // s
    {0x00U, 0x18U, 0x18U, 0x7EU, 0x18U, 0x18U, 0x18U, 0x18U, 0x1AU, 0x0CU, 0x00U, 0x00U}, // t
    {0x00U, 0x00U, 0x00U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x3EU, 0x00U, 0x00U}, // u
    {0x00U, 0x00U, 0x00U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x3CU, 0x18U, 0x00U, 0x00U}, // v
    {0x00U, 0x00U, 0x00U, 0xC6U, 0xC6U, 0xC6U, 0xD6U, 0xD6U, 0xFEU, 0x6CU, 0x00U, 0x00U}, // w
    {0x00U, 0x00U, 0x00U, 0x66U, 0x66U, 0x3CU, 0x18U, 0x3CU, 0x66U, 0x66U, 0x00U, 0x00U}, // x
    {0x00U, 0x00U, 0x00U, 0x66U, 0x66U, 0x66U, 0x66U, 0x3EU, 0x06U, 0x66U, 0x3CU, 0x00U}, // y
    {0x00U, 0x00U, 0x00U, 0x7EU, 0x06U, 0x0CU, 0x18U, 0x30U, 0x60U, 0x7EU, 0x00U, 0x00U}, // z
    {0x00U, 0x3CU, 0x66U, 0x06U, 0x0CU, 0x18U, 0x18U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U}, // ?
    {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}, // space
    {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U}, // .
    {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x18U, 0x30U, 0x00U}, // ,
    {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}, // -
    {0x00U, 0x18U, 0x18U, 0x30U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}, // '
    {0x00U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U, 0x00U}, // :
};

// === Private function implementation ============================================================================= //

/**
 * @brief Return the 8x12 glyph bitmap for one supported character.
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
    if ((x >= SUBTITLE_BRAM_MASK_WIDTH) || (y >= SUBTITLE_BRAM_MASK_HEIGHT))
    {
        return;
    }

    size_t const byte_index = ((size_t)y * RENDER_BITMAP_STRIDE) + ((size_t)x / 8U);
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
static void draw_glyph(uint8_t* const dst,
                       uint32_t x,
                       uint32_t y,
                       uint8_t const* const glyph,
                       uint8_t const dimmed)
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
                        if ((dimmed != 0U) && (GLYPH_SCALE > 1U)
                            && (scaled_row == (GLYPH_SCALE - 1U))
                            && (scaled_col == (GLYPH_SCALE - 1U)))
                        {
                            continue;
                        }
                        set_bitmap_pixel(dst,
                                         x + (col * GLYPH_SCALE) + scaled_col,
                                         y + (row * GLYPH_SCALE) + scaled_row);
                    }
                }
            }
        }
    }
}

static void append_layout_line(char lines[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                               uint32_t* const line_count,
                               char const* const line)
{
    if (line[0] == '\0')
    {
        return;
    }

    if (*line_count >= RENDER_LAYOUT_MAX_LINES)
    {
        memmove(lines[0], lines[1], (RENDER_LAYOUT_MAX_LINES - 1U) * sizeof(lines[0]));
        *line_count = RENDER_LAYOUT_MAX_LINES - 1U;
    }

    strncpy(lines[*line_count], line, RENDER_LINE_TEXT_MAX);
    lines[*line_count][RENDER_LINE_TEXT_MAX] = '\0';
    (*line_count)++;
}

static void append_word(char lines[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                        uint32_t* const line_count,
                        char current[RENDER_GLYPHS_PER_LINE + 1U],
                        char const* word,
                        size_t word_len)
{
    while (word_len > 0U)
    {
        size_t const current_len = strlen(current);

        if (current_len == 0U)
        {
            size_t const copy_len = (word_len > RENDER_LINE_TEXT_MAX) ? (RENDER_LINE_TEXT_MAX - 1U)
                                                                      : word_len;

            memcpy(current, word, copy_len);
            current[copy_len] = (word_len > RENDER_LINE_TEXT_MAX) ? '-' : '\0';
            current[copy_len + ((word_len > RENDER_LINE_TEXT_MAX) ? 1U : 0U)] = '\0';
            word += copy_len;
            word_len -= copy_len;

            if (word_len > 0U)
            {
                append_layout_line(lines, line_count, current);
                current[0] = '\0';
            }
        }
        else if ((current_len + 1U + word_len) <= RENDER_LINE_TEXT_MAX)
        {
            current[current_len] = ' ';
            memcpy(&current[current_len + 1U], word, word_len);
            current[current_len + 1U + word_len] = '\0';
            word_len = 0U;
        }
        else
        {
            append_layout_line(lines, line_count, current);
            current[0] = '\0';
        }
    }
}

static int sanitize_slice(char const* const src, size_t len, char* const dst, size_t const dst_size)
{
    char raw[RENDER_SANITIZE_MAX];

    if (len >= sizeof(raw))
    {
        len = sizeof(raw) - 1U;
    }

    memcpy(raw, src, len);
    raw[len] = '\0';
    return subtitle_text_sanitize(raw, dst, dst_size);
}

static uint32_t
build_wrapped_lines(char const* const text,
                    char lines[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U])
{
    char current[RENDER_GLYPHS_PER_LINE + 1U] = {0};
    uint32_t line_count = 0U;
    char const* cursor = text;

    while (*cursor != '\0')
    {
        char const* word;
        size_t word_len;

        while (*cursor == ' ')
        {
            cursor++;
        }

        word = cursor;
        while ((*cursor != '\0') && (*cursor != ' '))
        {
            cursor++;
        }

        word_len = (size_t)(cursor - word);
        append_word(lines, &line_count, current, word, word_len);
    }

    append_layout_line(lines, &line_count, current);
    return line_count;
}

static uint32_t copy_tail_lines(char dst[RENDER_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                                uint8_t dst_roles[RENDER_MAX_LINES],
                                uint32_t dst_start,
                                uint32_t const max_lines,
                                uint8_t const role,
                                char src[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                                uint32_t const src_count)
{
    uint32_t const copy_count = (src_count > max_lines) ? max_lines : src_count;
    uint32_t const src_start = src_count - copy_count;
    uint32_t i;

    for (i = 0U; i < copy_count; i++)
    {
        snprintf(dst[dst_start], RENDER_GLYPHS_PER_LINE + 1U, "%s", src[src_start + i]);
        dst_roles[dst_start] = role;
        dst_start++;
    }

    return dst_start;
}

static int build_visible_lines(char const* const text,
                               char lines[RENDER_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U],
                               uint8_t line_roles[RENDER_MAX_LINES],
                               uint32_t* const line_count)
{
    char const* const split = strchr(text, '\n');
    char sanitized[RENDER_SANITIZE_MAX];
    char wrapped[RENDER_LAYOUT_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U] = {{0}};

    *line_count = 0U;

    if (split == NULL)
    {
        if (subtitle_text_sanitize(text, sanitized, sizeof(sanitized)) != 0)
        {
            return -EINVAL;
        }

        *line_count = copy_tail_lines(lines,
                                      line_roles,
                                      0U,
                                      RENDER_MAX_LINES,
                                      RENDER_LINE_CURRENT,
                                      wrapped,
                                      build_wrapped_lines(sanitized, wrapped));
        return 0;
    }

    if (sanitize_slice(text, (size_t)(split - text), sanitized, sizeof(sanitized)) != 0)
    {
        return -EINVAL;
    }
    *line_count = copy_tail_lines(lines,
                                  line_roles,
                                  *line_count,
                                  RENDER_PREVIOUS_LINES,
                                  RENDER_LINE_PREVIOUS,
                                  wrapped,
                                  build_wrapped_lines(sanitized, wrapped));

    memset(wrapped, 0, sizeof(wrapped));
    if (subtitle_text_sanitize(split + 1, sanitized, sizeof(sanitized)) != 0)
    {
        return -EINVAL;
    }
    *line_count = copy_tail_lines(lines,
                                  line_roles,
                                  *line_count,
                                  RENDER_CURRENT_LINES,
                                  RENDER_LINE_CURRENT,
                                  wrapped,
                                  build_wrapped_lines(sanitized, wrapped));

    return 0;
}

static void draw_text_line(uint8_t* const dst,
                           char const* const text,
                           uint32_t const line,
                           uint8_t const dimmed)
{
    uint32_t x = RENDER_TEXT_X;
    uint32_t const y = RENDER_TEXT_Y + (line * RENDER_LINE_HEIGHT);
    char const* cursor;

    for (cursor = text;
         (*cursor != '\0') && ((x + GLYPH_RENDERED_WIDTH) <= SUBTITLE_BRAM_MASK_WIDTH);
         cursor++)
    {
        draw_glyph(dst, x, y, glyph_for_char(*cursor), dimmed);
        x += GLYPH_ADVANCE;
    }
}

// === Public function implementation ============================================================================== //

/**
 * @brief Render text into a packed subtitle mask bitmap.
 * @param text Null-terminated text to render.
 * @param dst Destination bitmap buffer.
 * @param dst_size Destination buffer size in bytes.
 * @param width Rendered bitmap width destination.
 * @param height Rendered bitmap height destination.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int subtitle_text_renderer_render(char const* const text,
                                  uint8_t* const dst,
                                  size_t dst_size,
                                  uint32_t* const width,
                                  uint32_t* const height)
{
    return subtitle_text_renderer_render_caption(text, 1U, dst, dst_size, width, height);
}

int subtitle_text_renderer_render_caption(char const* const text,
                                          uint8_t const current_is_final,
                                          uint8_t* const dst,
                                          size_t dst_size,
                                          uint32_t* const width,
                                          uint32_t* const height)
{
    char lines[RENDER_MAX_LINES][RENDER_GLYPHS_PER_LINE + 1U] = {{0}};
    uint8_t line_roles[RENDER_MAX_LINES] = {0};
    uint32_t line_count;
    uint32_t visible_line;

    if ((text == NULL) || (dst == NULL) || (width == NULL) || (height == NULL)
        || (dst_size < RENDER_BITMAP_SIZE))
    {
        return -EINVAL;
    }

    if (build_visible_lines(text, lines, line_roles, &line_count) != 0)
    {
        return -EINVAL;
    }

    memset(dst, 0, dst_size);

    for (visible_line = 0U; visible_line < line_count; visible_line++)
    {
        uint8_t const dimmed =
            ((current_is_final == 0U) && (line_roles[visible_line] == RENDER_LINE_CURRENT)) ? 1U
                                                                                            : 0U;
        draw_text_line(dst, lines[visible_line], visible_line, dimmed);
    }

    *width = SUBTITLE_BRAM_MASK_WIDTH;
    *height = SUBTITLE_BRAM_MASK_HEIGHT;
    return 0;
}

// === End of documentation ======================================================================================== //
