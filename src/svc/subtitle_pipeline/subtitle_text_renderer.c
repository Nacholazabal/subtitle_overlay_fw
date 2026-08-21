/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file subtitle_text_renderer.c
/// @brief Proportional UTF-8 subtitle renderer for the hardware overlay mask
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_text_renderer.h"

#include <limits.h>
#include <string.h>

#include "errorno.h"
#include "subtitle_bram.h"
#include "subtitle_font.h"
#include "subtitle_text_sanitize.h"

// === Macros definitions ========================================================================================== //

#define RENDER_PADDING_X          (18U)
#define RENDER_PADDING_Y          (10U)
#define RENDER_MAX_LINES          (3U)
#define RENDER_PREVIOUS_LINES     (1U)
#define RENDER_CURRENT_LINES      (2U)
#define RENDER_LAYOUT_MAX_LINES   (32U)
#define RENDER_CODEPOINT_MAX      (512U)
#define RENDER_UTF8_MAX           (1024U)
#define RENDER_CONTENT_MAX_WIDTH  (SUBTITLE_BRAM_MASK_WIDTH - (2U * RENDER_PADDING_X))
#define RENDER_BITMAP_SIZE        (SUBTITLE_BRAM_SIZE_BYTES)
#define RENDER_ROLE_PREVIOUS      (0U)
#define RENDER_ROLE_CURRENT       (1U)

// === Private data type declarations ============================================================================== //

typedef struct
{
    uint16_t start;
    uint16_t count;
    uint8_t append_hyphen;
} render_span_t;

typedef struct
{
    uint32_t codepoints[RENDER_CODEPOINT_MAX];
    render_span_t lines[RENDER_LAYOUT_MAX_LINES];
    uint16_t codepoint_count;
    uint16_t line_count;
} render_layout_t;

typedef struct
{
    render_layout_t const* layout;
    render_span_t const* span;
    uint8_t role;
} visible_line_t;

typedef struct
{
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
} ink_bounds_t;

typedef struct
{
    ink_bounds_t lines[RENDER_MAX_LINES];
    int32_t min_y;
    int32_t max_y;
    uint32_t maximum_width;
} render_metrics_t;

// === Private function declarations =============================================================================== //

static int sanitize_slice(char const* source, size_t length, char* destination, size_t capacity);
static uint32_t decode_codepoint(char const* text, size_t* offset);
static int decode_text(char const* text, render_layout_t* layout);
static uint32_t span_advance(render_layout_t const* layout, uint16_t start, uint16_t count);
static void append_line(render_layout_t* layout, uint16_t start, uint16_t count, uint8_t hyphen);
static void split_word(render_layout_t* layout, uint16_t start, uint16_t end);
static void wrap_codepoints(render_layout_t* layout);
static int build_layout(char const* source, size_t length, render_layout_t* layout);
static uint32_t append_tail(visible_line_t* visible,
                            uint32_t count,
                            render_layout_t const* layout,
                            uint32_t maximum,
                            uint8_t role);
static uint32_t build_visible_lines(char const* text,
                                    render_layout_t* previous,
                                    render_layout_t* current,
                                    visible_line_t visible[RENDER_MAX_LINES]);
static ink_bounds_t measure_line(visible_line_t const* line, uint32_t baseline);
static void measure_visible(visible_line_t const* visible,
                            uint32_t line_count,
                            render_metrics_t* metrics);
static void set_pixel(uint8_t* bitmap, uint32_t stride, uint32_t x, uint32_t y);
static void draw_glyph(uint8_t* bitmap,
                       uint32_t stride,
                       uint32_t box_width,
                       uint32_t box_height,
                       subtitle_font_glyph_t const* glyph,
                       int32_t x,
                       int32_t y,
                       uint8_t dimmed);
static void draw_line(uint8_t* bitmap,
                      uint32_t stride,
                      uint32_t box_width,
                      uint32_t box_height,
                      visible_line_t const* line,
                      int32_t pen_x,
                      int32_t baseline,
                      uint8_t dimmed);
static void draw_visible(uint8_t* bitmap,
                         uint32_t width,
                         uint32_t height,
                         visible_line_t const* visible,
                         uint32_t line_count,
                         render_metrics_t const* metrics,
                         uint8_t current_is_final);

// === Private function implementation ============================================================================= //

static int sanitize_slice(char const* const source,
                          size_t const length,
                          char* const destination,
                          size_t const capacity)
{
    char input[RENDER_UTF8_MAX];

    if ((length + 1U) > sizeof(input))
    {
        return -EINVAL;
    }

    memcpy(input, source, length);
    input[length] = '\0';
    return subtitle_text_sanitize(input, destination, capacity);
}

static uint32_t decode_codepoint(char const* const text, size_t* const offset)
{
    unsigned char const lead = (unsigned char)text[*offset];

    if (lead < 0x80U)
    {
        (*offset)++;
        return lead;
    }

    {
        unsigned char const continuation = (unsigned char)text[*offset + 1U];
        *offset += 2U;
        return ((uint32_t)(lead & 0x1FU) << 6U) | (uint32_t)(continuation & 0x3FU);
    }
}

static int decode_text(char const* const text, render_layout_t* const layout)
{
    size_t offset = 0U;
    uint8_t pending_space = 0U;

    while ((text[offset] != '\0') && (layout->codepoint_count < RENDER_CODEPOINT_MAX))
    {
        uint32_t const codepoint = decode_codepoint(text, &offset);

        if (codepoint == (uint32_t)' ')
        {
            pending_space = (layout->codepoint_count != 0U) ? 1U : 0U;
        }
        else
        {
            uint16_t const required = (pending_space != 0U) ? 2U : 1U;

            if (layout->codepoint_count > (RENDER_CODEPOINT_MAX - required))
            {
                break;
            }
            if (pending_space != 0U)
            {
                layout->codepoints[layout->codepoint_count++] = (uint32_t)' ';
            }
            layout->codepoints[layout->codepoint_count++] = codepoint;
            pending_space = 0U;
        }
    }

    return (layout->codepoint_count != 0U) ? 0 : -EINVAL;
}

static uint32_t span_advance(render_layout_t const* const layout,
                             uint16_t const start,
                             uint16_t const count)
{
    uint32_t width = 0U;
    uint16_t index;

    for (index = 0U; index < count; index++)
    {
        width += subtitle_font_lookup(layout->codepoints[start + index])->advance;
    }
    return width;
}

static void append_line(render_layout_t* const layout,
                        uint16_t const start,
                        uint16_t const count,
                        uint8_t const hyphen)
{
    if ((count == 0U) || (layout->line_count >= RENDER_LAYOUT_MAX_LINES))
    {
        return;
    }

    layout->lines[layout->line_count].start = start;
    layout->lines[layout->line_count].count = count;
    layout->lines[layout->line_count].append_hyphen = hyphen;
    layout->line_count++;
}

static void split_word(render_layout_t* const layout, uint16_t start, uint16_t const end)
{
    uint32_t const hyphen_width = subtitle_font_lookup((uint32_t)'-')->advance;

    while (start < end)
    {
        uint16_t count = 0U;
        uint32_t width = 0U;
        uint32_t const remaining_width = span_advance(layout, start, (uint16_t)(end - start));

        if (remaining_width <= RENDER_CONTENT_MAX_WIDTH)
        {
            append_line(layout, start, (uint16_t)(end - start), 0U);
            return;
        }

        while ((start + count) < end)
        {
            uint32_t const advance = subtitle_font_lookup(layout->codepoints[start + count])->advance;
            if ((count != 0U) && ((width + advance + hyphen_width) > RENDER_CONTENT_MAX_WIDTH))
            {
                break;
            }
            width += advance;
            count++;
        }

        append_line(layout, start, count, 1U);
        start = (uint16_t)(start + count);
    }
}

static void wrap_codepoints(render_layout_t* const layout)
{
    uint16_t cursor = 0U;
    uint16_t line_start = 0U;
    uint16_t line_count = 0U;
    uint32_t line_width = 0U;
    uint32_t const space_width = subtitle_font_lookup((uint32_t)' ')->advance;

    while (cursor < layout->codepoint_count)
    {
        uint16_t const word_start = cursor;
        uint16_t word_end;
        uint32_t word_width;

        while ((cursor < layout->codepoint_count) && (layout->codepoints[cursor] != (uint32_t)' '))
        {
            cursor++;
        }
        word_end = cursor;
        word_width = span_advance(layout, word_start, (uint16_t)(word_end - word_start));

        if ((line_count != 0U) && ((line_width + space_width + word_width) > RENDER_CONTENT_MAX_WIDTH))
        {
            append_line(layout, line_start, line_count, 0U);
            line_count = 0U;
            line_width = 0U;
        }
        if ((line_count == 0U) && (word_width > RENDER_CONTENT_MAX_WIDTH))
        {
            split_word(layout, word_start, word_end);
        }
        else
        {
            line_start = (line_count == 0U) ? word_start : line_start;
            line_count = (uint16_t)(word_end - line_start);
            line_width = (line_width == 0U) ? word_width : (line_width + space_width + word_width);
        }
        cursor = (cursor < layout->codepoint_count) ? (uint16_t)(cursor + 1U) : cursor;
    }

    append_line(layout, line_start, line_count, 0U);
}

static int build_layout(char const* const source,
                        size_t const length,
                        render_layout_t* const layout)
{
    char normalized[RENDER_UTF8_MAX];
    int status;

    memset(layout, 0, sizeof(*layout));
    status = sanitize_slice(source, length, normalized, sizeof(normalized));
    if (status == 0)
    {
        status = decode_text(normalized, layout);
    }
    if (status == 0)
    {
        wrap_codepoints(layout);
    }
    return status;
}

static uint32_t append_tail(visible_line_t* const visible,
                            uint32_t count,
                            render_layout_t const* const layout,
                            uint32_t const maximum,
                            uint8_t const role)
{
    uint32_t source = (layout->line_count > maximum) ? (layout->line_count - maximum) : 0U;

    while ((source < layout->line_count) && (count < RENDER_MAX_LINES))
    {
        visible[count].layout = layout;
        visible[count].span = &layout->lines[source];
        visible[count].role = role;
        count++;
        source++;
    }
    return count;
}

static uint32_t build_visible_lines(char const* const text,
                                    render_layout_t* const previous,
                                    render_layout_t* const current,
                                    visible_line_t visible[RENDER_MAX_LINES])
{
    char const* const separator = strchr(text, '\n');
    uint32_t count = 0U;

    if (separator != NULL)
    {
        if (build_layout(text, (size_t)(separator - text), previous) == 0)
        {
            count = append_tail(visible, count, previous, RENDER_PREVIOUS_LINES, RENDER_ROLE_PREVIOUS);
        }
        if (build_layout(separator + 1, strlen(separator + 1), current) == 0)
        {
            count = append_tail(visible, count, current, RENDER_CURRENT_LINES, RENDER_ROLE_CURRENT);
        }
    }
    else if (build_layout(text, strlen(text), current) == 0)
    {
        count = append_tail(visible, count, current, RENDER_MAX_LINES, RENDER_ROLE_CURRENT);
    }
    return count;
}

static ink_bounds_t measure_line(visible_line_t const* const line, uint32_t const baseline)
{
    ink_bounds_t bounds = {INT_MAX, INT_MIN, INT_MAX, INT_MIN};
    uint32_t pen = 0U;
    uint16_t index;
    uint16_t const total = (uint16_t)(line->span->count + line->span->append_hyphen);

    for (index = 0U; index < total; index++)
    {
        uint32_t const codepoint = (index < line->span->count)
                                       ? line->layout->codepoints[line->span->start + index]
                                       : (uint32_t)'-';
        subtitle_font_glyph_t const* const glyph = subtitle_font_lookup(codepoint);

        if ((glyph->bitmap != NULL) && (glyph->width != 0U) && (glyph->height != 0U))
        {
            int32_t const left = (int32_t)pen + glyph->x_offset;
            int32_t const top = (int32_t)baseline + glyph->y_offset;
            bounds.min_x = (left < bounds.min_x) ? left : bounds.min_x;
            bounds.max_x = ((left + glyph->width - 1) > bounds.max_x) ? (left + glyph->width - 1) : bounds.max_x;
            bounds.min_y = (top < bounds.min_y) ? top : bounds.min_y;
            bounds.max_y = ((top + glyph->height - 1) > bounds.max_y) ? (top + glyph->height - 1) : bounds.max_y;
        }
        pen += glyph->advance;
    }
    return bounds;
}

static void measure_visible(visible_line_t const* const visible,
                            uint32_t const line_count,
                            render_metrics_t* const metrics)
{
    uint32_t line;

    metrics->min_y = INT_MAX;
    metrics->max_y = INT_MIN;
    metrics->maximum_width = 0U;
    for (line = 0U; line < line_count; line++)
    {
        uint32_t const baseline = line * subtitle_font_line_height();
        uint32_t ink_width;

        metrics->lines[line] = measure_line(&visible[line], baseline);
        ink_width = (uint32_t)(metrics->lines[line].max_x - metrics->lines[line].min_x + 1);
        metrics->maximum_width = (ink_width > metrics->maximum_width) ? ink_width
                                                                      : metrics->maximum_width;
        metrics->min_y = (metrics->lines[line].min_y < metrics->min_y)
                             ? metrics->lines[line].min_y
                             : metrics->min_y;
        metrics->max_y = (metrics->lines[line].max_y > metrics->max_y)
                             ? metrics->lines[line].max_y
                             : metrics->max_y;
    }
}

static void set_pixel(uint8_t* const bitmap,
                      uint32_t const stride,
                      uint32_t const x,
                      uint32_t const y)
{
    bitmap[((size_t)y * stride) + (x / 8U)] |= (uint8_t)(1U << (7U - (x % 8U)));
}

static void draw_glyph(uint8_t* const bitmap,
                       uint32_t const stride,
                       uint32_t const box_width,
                       uint32_t const box_height,
                       subtitle_font_glyph_t const* const glyph,
                       int32_t const x,
                       int32_t const y,
                       uint8_t const dimmed)
{
    uint32_t const glyph_stride = (glyph->width + 7U) / 8U;
    uint32_t row;

    for (row = 0U; row < glyph->height; row++)
    {
        uint32_t column;
        for (column = 0U; column < glyph->width; column++)
        {
            int32_t const destination_x = x + (int32_t)column;
            int32_t const destination_y = y + (int32_t)row;
            uint8_t const source = glyph->bitmap[(row * glyph_stride) + (column / 8U)];

            if ((source & (1U << (7U - (column % 8U)))) == 0U)
            {
                continue;
            }
            if ((destination_x < 0) || (destination_y < 0)
                || ((uint32_t)destination_x >= box_width) || ((uint32_t)destination_y >= box_height))
            {
                continue;
            }
            if ((dimmed != 0U) && (((uint32_t)destination_x & 1U) != 0U)
                && (((uint32_t)destination_y & 1U) != 0U))
            {
                continue;
            }
            set_pixel(bitmap, stride, (uint32_t)destination_x, (uint32_t)destination_y);
        }
    }
}

static void draw_line(uint8_t* const bitmap,
                      uint32_t const stride,
                      uint32_t const box_width,
                      uint32_t const box_height,
                      visible_line_t const* const line,
                      int32_t pen_x,
                      int32_t const baseline,
                      uint8_t const dimmed)
{
    uint16_t index;
    uint16_t const total = (uint16_t)(line->span->count + line->span->append_hyphen);

    for (index = 0U; index < total; index++)
    {
        uint32_t const codepoint = (index < line->span->count)
                                       ? line->layout->codepoints[line->span->start + index]
                                       : (uint32_t)'-';
        subtitle_font_glyph_t const* const glyph = subtitle_font_lookup(codepoint);

        if (glyph->bitmap != NULL)
        {
            draw_glyph(bitmap,
                       stride,
                       box_width,
                       box_height,
                       glyph,
                       pen_x + glyph->x_offset,
                       baseline + glyph->y_offset,
                       dimmed);
        }
        pen_x += glyph->advance;
    }
}

static void draw_visible(uint8_t* const bitmap,
                         uint32_t const width,
                         uint32_t const height,
                         visible_line_t const* const visible,
                         uint32_t const line_count,
                         render_metrics_t const* const metrics,
                         uint8_t const current_is_final)
{
    uint32_t const stride = (width + 7U) / 8U;
    uint32_t line;

    for (line = 0U; line < line_count; line++)
    {
        uint32_t const ink_width = (uint32_t)(metrics->lines[line].max_x
                                              - metrics->lines[line].min_x + 1);
        int32_t const pen_x = (int32_t)RENDER_PADDING_X
                              + (int32_t)((metrics->maximum_width - ink_width) / 2U)
                              - metrics->lines[line].min_x;
        int32_t const baseline = (int32_t)RENDER_PADDING_Y - metrics->min_y
                                 + (int32_t)(line * subtitle_font_line_height());
        uint8_t const dimmed = ((visible[line].role == RENDER_ROLE_CURRENT)
                                && (current_is_final == 0U))
                                   ? 1U
                                   : 0U;

        draw_line(bitmap, stride, width, height, &visible[line], pen_x, baseline, dimmed);
    }
}

// === Public function implementation ============================================================================== //

/**
 * @brief Render plain UTF-8 subtitle text as a final caption.
 * @param text Null-terminated subtitle text.
 * @param dst Destination packed MSB-first 1-bpp bitmap.
 * @param dst_size Destination capacity; must hold the complete hardware mask.
 * @param width Rendered compact box width, including 18 px horizontal padding per side.
 * @param height Rendered compact box height, including 10 px vertical padding per side.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int subtitle_text_renderer_render(char const* const text,
                                  uint8_t* const dst,
                                  size_t const dst_size,
                                  uint32_t* const width,
                                  uint32_t* const height)
{
    return subtitle_text_renderer_render_caption(text, 1U, dst, dst_size, width, height);
}

/**
 * @brief Render a final/partial broadcast caption using proportional Lato Semibold glyphs.
 *
 * A caption pair reserves one line for the previous final segment and up to two lines for
 * the current segment. Partial current text is dithered; previous and final text remain solid.
 * The returned bitmap is tightly packed to a black box with 18 px horizontal and 10 px vertical
 * padding, and never exceeds the 1024x256 hardware mask.
 * @param text Null-terminated UTF-8 text; the first newline separates previous and current text.
 * @param current_is_final Nonzero renders the current segment solid.
 * @param dst Destination packed MSB-first 1-bpp bitmap.
 * @param dst_size Destination capacity; must hold the complete hardware mask.
 * @param width Rendered compact box width in pixels.
 * @param height Rendered compact box height in pixels.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int subtitle_text_renderer_render_caption(char const* const text,
                                          uint8_t const current_is_final,
                                          uint8_t* const dst,
                                          size_t const dst_size,
                                          uint32_t* const width,
                                          uint32_t* const height)
{
    render_layout_t previous;
    render_layout_t current;
    visible_line_t visible[RENDER_MAX_LINES];
    render_metrics_t metrics;
    uint32_t line_count;

    if ((text == NULL) || (dst == NULL) || (width == NULL) || (height == NULL)
        || (dst_size < RENDER_BITMAP_SIZE))
    {
        return -EINVAL;
    }

    line_count = build_visible_lines(text, &previous, &current, visible);
    if (line_count == 0U)
    {
        return -EINVAL;
    }

    measure_visible(visible, line_count, &metrics);
    *width = metrics.maximum_width + (2U * RENDER_PADDING_X);
    *height = (uint32_t)(metrics.max_y - metrics.min_y + 1) + (2U * RENDER_PADDING_Y);
    if ((*width > SUBTITLE_BRAM_MASK_WIDTH) || (*height > SUBTITLE_BRAM_MASK_HEIGHT))
    {
        return -EINVAL;
    }

    memset(dst, 0, dst_size);
    draw_visible(dst, *width, *height, visible, line_count, &metrics, current_is_final);
    return 0;
}

// === End of documentation ======================================================================================== //
