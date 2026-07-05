#include <string.h>

#include "unity.h"
#include "errorno.h"
#include "subtitle_text_renderer.h"
#include "subtitle_text_sanitize.h"

#define TEST_MASK_WIDTH      (1024U)
#define TEST_MASK_HEIGHT     (256U)
#define TEST_MASK_SIZE_BYTES ((TEST_MASK_WIDTH * TEST_MASK_HEIGHT) / 8U)
#define TEST_GLYPH_WIDTH     (8U)
#define TEST_GLYPH_HEIGHT    (12U)
#define TEST_GLYPH_SCALE     (2U)
#define TEST_GLYPH_ADVANCE   (9U * TEST_GLYPH_SCALE)
#define TEST_RENDERED_HEIGHT (TEST_GLYPH_HEIGHT * TEST_GLYPH_SCALE)
#define TEST_LINE_HEIGHT     (16U * TEST_GLYPH_SCALE)
#define TEST_BLOCK_HEIGHT    (TEST_RENDERED_HEIGHT + (2U * TEST_LINE_HEIGHT))
#define TEST_TEXT_X          (24U)
#define TEST_TEXT_Y          ((TEST_MASK_HEIGHT - TEST_BLOCK_HEIGHT) / 2U)
#define TEST_LINE_CAPACITY   (55U)

static uint8_t bitmap[TEST_MASK_SIZE_BYTES];

static uint8_t pixel_is_set(uint32_t x, uint32_t y)
{
    size_t const stride = (size_t)TEST_MASK_WIDTH / 8U;
    size_t const byte_index = ((size_t)y * stride) + ((size_t)x / 8U);
    uint8_t const bit_mask = (uint8_t)(1U << (7U - (x % 8U)));

    return (bitmap[byte_index] & bit_mask) != 0U;
}

static uint8_t row_has_pixels(uint32_t row)
{
    size_t const stride = (size_t)TEST_MASK_WIDTH / 8U;
    size_t const offset = (size_t)row * stride;
    size_t col;

    for (col = 0U; col < stride; col++)
    {
        if (bitmap[offset + col] != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

static uint8_t rect_has_pixels(uint32_t x0, uint32_t y0, uint32_t width, uint32_t height)
{
    uint32_t y;

    for (y = y0; (y < (y0 + height)) && (y < TEST_MASK_HEIGHT); y++)
    {
        uint32_t x;

        for (x = x0; (x < (x0 + width)) && (x < TEST_MASK_WIDTH); x++)
        {
            if (pixel_is_set(x, y) != 0U)
            {
                return 1U;
            }
        }
    }

    return 0U;
}

static uint8_t line_band_has_pixels(uint32_t line)
{
    uint32_t const y0 = TEST_TEXT_Y + (line * TEST_LINE_HEIGHT);
    uint32_t row;

    for (row = y0; row < (y0 + TEST_RENDERED_HEIGHT); row++)
    {
        if (row_has_pixels(row) != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

static uint32_t line_pixel_count(uint32_t line)
{
    uint32_t const y0 = TEST_TEXT_Y + (line * TEST_LINE_HEIGHT);
    uint32_t count = 0U;
    uint32_t y;

    for (y = y0; y < (y0 + TEST_RENDERED_HEIGHT); y++)
    {
        uint32_t x;

        for (x = 0U; x < TEST_MASK_WIDTH; x++)
        {
            if (pixel_is_set(x, y) != 0U)
            {
                count++;
            }
        }
    }

    return count;
}

static uint32_t rendered_line_bands(void)
{
    uint32_t lines = 0U;
    uint32_t line;

    for (line = 0U; line < 3U; line++)
    {
        if (line_band_has_pixels(line) != 0U)
        {
            lines++;
        }
    }

    return lines;
}

static uint32_t first_line_max_x(void)
{
    uint32_t max_x = 0U;
    uint32_t row;

    for (row = TEST_TEXT_Y; row < (TEST_TEXT_Y + TEST_RENDERED_HEIGHT); row++)
    {
        uint32_t x;

        for (x = 0U; x < TEST_MASK_WIDTH; x++)
        {
            if (pixel_is_set(x, row) != 0U)
            {
                max_x = x;
            }
        }
    }

    return max_x;
}

void setUp(void)
{
    memset(bitmap, 0xAA, sizeof(bitmap));
}

void tearDown(void)
{}

void test_subtitle_text_renderer_rejects_invalid_arguments(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;

    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render(NULL, bitmap, sizeof(bitmap), &width, &height));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render("hola", NULL, sizeof(bitmap), &width, &height));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render("hola", bitmap, sizeof(bitmap) - 1U, &width, &height));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render_caption("hola", 0U, bitmap, sizeof(bitmap) - 1U, &width, &height));
}

void test_subtitle_text_renderer_limits_plain_text_to_three_lines(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    char const* const text =
        "uno dos tres cuatro cinco seis siete ocho nueve diez once doce trece catorce quince "
        "dieciseis diecisiete dieciocho diecinueve veinte veintiuno veintidos veintitres";

    TEST_ASSERT_EQUAL_INT(0, subtitle_text_renderer_render(text, bitmap, sizeof(bitmap), &width, &height));

    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_WIDTH, width);
    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_HEIGHT, height);
    TEST_ASSERT_TRUE(rendered_line_bands() <= 3U);
}

void test_subtitle_text_renderer_limits_broadcast_pair_to_three_lines(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    char const* const text =
        "segmento anterior con varias palabras que deberia ocupar solo una linea\n"
        "segmento actual bastante mas largo que deberia usar como maximo dos lineas visibles";

    TEST_ASSERT_EQUAL_INT(0, subtitle_text_renderer_render(text, bitmap, sizeof(bitmap), &width, &height));

    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_WIDTH, width);
    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_HEIGHT, height);
    TEST_ASSERT_TRUE(rendered_line_bands() <= 3U);
}

void test_subtitle_text_renderer_does_not_cut_normal_words(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    char const* const text =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa bbbbbbbbbb";
    uint32_t const first_word_last_x = TEST_TEXT_X + ((50U - 1U) * TEST_GLYPH_ADVANCE) + 15U;

    TEST_ASSERT_EQUAL_INT(0, subtitle_text_renderer_render(text, bitmap, sizeof(bitmap), &width, &height));

    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_WIDTH, width);
    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_HEIGHT, height);
    TEST_ASSERT_EQUAL_UINT32(2U, rendered_line_bands());
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(first_word_last_x, first_line_max_x());
}

void test_subtitle_text_renderer_hyphenates_single_oversized_word(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    char const* const text =
        "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii";
    uint32_t const hyphen_x = TEST_TEXT_X + ((TEST_LINE_CAPACITY - 1U) * TEST_GLYPH_ADVANCE);
    uint32_t const hyphen_width = TEST_GLYPH_WIDTH * TEST_GLYPH_SCALE;

    TEST_ASSERT_EQUAL_INT(0, subtitle_text_renderer_render(text, bitmap, sizeof(bitmap), &width, &height));

    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_WIDTH, width);
    TEST_ASSERT_EQUAL_UINT32(TEST_MASK_HEIGHT, height);
    TEST_ASSERT_TRUE(rendered_line_bands() <= 3U);
    TEST_ASSERT_TRUE(rect_has_pixels(hyphen_x, TEST_TEXT_Y, hyphen_width, TEST_RENDERED_HEIGHT) != 0U);
}

void test_subtitle_text_renderer_dims_only_current_partial_lines(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t previous_pixels_final;
    uint32_t current_pixels_final;
    uint32_t previous_pixels_partial;
    uint32_t current_pixels_partial;

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render_caption("previo\nactual", 1U, bitmap, sizeof(bitmap), &width, &height));
    previous_pixels_final = line_pixel_count(0U);
    current_pixels_final = line_pixel_count(1U);

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render_caption("previo\nactual", 0U, bitmap, sizeof(bitmap), &width, &height));
    previous_pixels_partial = line_pixel_count(0U);
    current_pixels_partial = line_pixel_count(1U);

    TEST_ASSERT_EQUAL_UINT32(previous_pixels_final, previous_pixels_partial);
    TEST_ASSERT_TRUE(current_pixels_partial < current_pixels_final);
    TEST_ASSERT_TRUE(current_pixels_partial > 0U);
}
