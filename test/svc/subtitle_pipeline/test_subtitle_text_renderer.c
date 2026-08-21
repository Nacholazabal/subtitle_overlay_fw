#include <string.h>

#include "unity.h"
#include "errorno.h"
#include "subtitle_bram.h"
#include "subtitle_font.h"
#include "subtitle_text_renderer.h"
#include "mock_hw_platform.h"

TEST_SOURCE_FILE("subtitle_font.c")
TEST_SOURCE_FILE("subtitle_text_sanitize.c")

static uint8_t bitmap[SUBTITLE_BRAM_SIZE_BYTES];
static uint32_t rendered_width;
static uint32_t rendered_height;

static uint8_t pixel_is_set(uint32_t const x, uint32_t const y)
{
    uint32_t const stride = (rendered_width + 7U) / 8U;
    size_t const index = ((size_t)y * stride) + (x / 8U);
    return ((bitmap[index] & (1U << (7U - (x % 8U)))) != 0U) ? 1U : 0U;
}

static uint32_t pixel_count(void)
{
    uint32_t count = 0U;
    uint32_t y;

    for (y = 0U; y < rendered_height; y++)
    {
        uint32_t x;
        for (x = 0U; x < rendered_width; x++)
        {
            count += pixel_is_set(x, y);
        }
    }
    return count;
}

static uint8_t row_has_pixels(uint32_t const y)
{
    uint32_t x;
    for (x = 0U; x < rendered_width; x++)
    {
        if (pixel_is_set(x, y) != 0U)
        {
            return 1U;
        }
    }
    return 0U;
}

static uint8_t column_has_pixels(uint32_t const x)
{
    uint32_t y;
    for (y = 0U; y < rendered_height; y++)
    {
        if (pixel_is_set(x, y) != 0U)
        {
            return 1U;
        }
    }
    return 0U;
}

static uint32_t ink_bands(void)
{
    uint32_t bands = 0U;
    uint8_t inside = 0U;
    uint32_t y;

    for (y = 0U; y < rendered_height; y++)
    {
        uint8_t const occupied = row_has_pixels(y);
        if ((occupied != 0U) && (inside == 0U))
        {
            bands++;
        }
        inside = occupied;
    }
    return bands;
}

static uint32_t first_band_pixels(void)
{
    uint32_t count = 0U;
    uint8_t started = 0U;
    uint32_t y;

    for (y = 0U; y < rendered_height; y++)
    {
        uint8_t const occupied = row_has_pixels(y);
        if ((started != 0U) && (occupied == 0U))
        {
            break;
        }
        if (occupied != 0U)
        {
            uint32_t x;
            started = 1U;
            for (x = 0U; x < rendered_width; x++)
            {
                count += pixel_is_set(x, y);
            }
        }
    }
    return count;
}

void setUp(void)
{
    memset(bitmap, 0xAA, sizeof(bitmap));
    rendered_width = 0U;
    rendered_height = 0U;
}

void tearDown(void)
{}

void test_subtitle_text_renderer_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render(NULL, bitmap, sizeof(bitmap), &rendered_width, &rendered_height));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render("hola", NULL, sizeof(bitmap), &rendered_width, &rendered_height));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render("hola",
                                      bitmap,
                                      sizeof(bitmap) - 1U,
                                      &rendered_width,
                                      &rendered_height));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render("   ", bitmap, sizeof(bitmap), &rendered_width, &rendered_height));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_text_renderer_render("", bitmap, sizeof(bitmap), &rendered_width, &rendered_height));
}

void test_subtitle_text_renderer_keeps_consecutive_c_glyphs_visually_separate(void)
{
    subtitle_font_glyph_t const* const glyph = subtitle_font_lookup((uint32_t)'c');
    uint32_t const first_left = 18U;
    uint32_t const first_right = first_left + glyph->width - 1U;
    uint32_t const second_left = first_left + glyph->advance;
    uint32_t x;

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render("cc", bitmap, sizeof(bitmap), &rendered_width, &rendered_height));

    TEST_ASSERT_EQUAL_UINT8(1U, column_has_pixels(first_right));
    TEST_ASSERT_EQUAL_UINT8(1U, column_has_pixels(second_left));
    TEST_ASSERT_TRUE(second_left > (first_right + 1U));
    for (x = first_right + 1U; x < second_left; x++)
    {
        TEST_ASSERT_EQUAL_UINT8(0U, column_has_pixels(x));
    }
}

void test_subtitle_text_renderer_returns_compact_geometry_with_selected_padding(void)
{
    uint32_t min_x = SUBTITLE_BRAM_MASK_WIDTH;
    uint32_t max_x = 0U;
    uint32_t min_y = SUBTITLE_BRAM_MASK_HEIGHT;
    uint32_t max_y = 0U;
    uint32_t y;

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render("Acción", bitmap, sizeof(bitmap), &rendered_width, &rendered_height));

    for (y = 0U; y < rendered_height; y++)
    {
        uint32_t x;
        for (x = 0U; x < rendered_width; x++)
        {
            if (pixel_is_set(x, y) != 0U)
            {
                min_x = (x < min_x) ? x : min_x;
                max_x = (x > max_x) ? x : max_x;
                min_y = (y < min_y) ? y : min_y;
                max_y = (y > max_y) ? y : max_y;
            }
        }
    }

    TEST_ASSERT_LESS_THAN_UINT32(SUBTITLE_BRAM_MASK_WIDTH, rendered_width);
    TEST_ASSERT_LESS_THAN_UINT32(SUBTITLE_BRAM_MASK_HEIGHT, rendered_height);
    TEST_ASSERT_EQUAL_UINT32(18U, min_x);
    TEST_ASSERT_EQUAL_UINT32(10U, min_y);
    TEST_ASSERT_EQUAL_UINT32(rendered_width - 19U, max_x);
    TEST_ASSERT_EQUAL_UINT32(rendered_height - 11U, max_y);
}

void test_subtitle_text_renderer_uses_proportional_advances(void)
{
    uint32_t narrow_width;

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render("iii", bitmap, sizeof(bitmap), &rendered_width, &rendered_height));
    narrow_width = rendered_width;
    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render("mmm", bitmap, sizeof(bitmap), &rendered_width, &rendered_height));

    TEST_ASSERT_TRUE(rendered_width > narrow_width);
}

void test_subtitle_text_renderer_draws_real_spanish_glyphs(void)
{
    uint8_t plain[SUBTITLE_BRAM_SIZE_BYTES];
    uint32_t plain_width;
    uint32_t plain_height;

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render("n", plain, sizeof(plain), &plain_width, &plain_height));
    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render("ñ", bitmap, sizeof(bitmap), &rendered_width, &rendered_height));

    TEST_ASSERT_TRUE(rendered_height > plain_height);
    TEST_ASSERT_TRUE(memcmp(plain, bitmap, sizeof(bitmap)) != 0);
}

void test_subtitle_text_renderer_limits_wrapped_text_to_three_lines(void)
{
    char const* const text =
        "uno dos tres cuatro cinco seis siete ocho nueve diez once doce trece catorce quince "
        "dieciseis diecisiete dieciocho diecinueve veinte veintiuno veintidos veintitres "
        "veinticuatro veinticinco veintiseis veintisiete veintiocho";

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render(text, bitmap, sizeof(bitmap), &rendered_width, &rendered_height));

    TEST_ASSERT_TRUE(ink_bands() <= 3U);
    TEST_ASSERT_TRUE(rendered_width <= SUBTITLE_BRAM_MASK_WIDTH);
    TEST_ASSERT_TRUE(rendered_height <= SUBTITLE_BRAM_MASK_HEIGHT);
}

void test_subtitle_text_renderer_hyphenates_oversized_words(void)
{
    char text[241];
    memset(text, 'W', sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render(text, bitmap, sizeof(bitmap), &rendered_width, &rendered_height));

    TEST_ASSERT_EQUAL_UINT32(3U, ink_bands());
    TEST_ASSERT_TRUE(rendered_width <= SUBTITLE_BRAM_MASK_WIDTH);
}

void test_subtitle_text_renderer_dims_only_current_partial_lines(void)
{
    uint32_t previous_final;
    uint32_t total_final;
    uint32_t previous_partial;
    uint32_t total_partial;

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render_caption(
            "previo\nactual", 1U, bitmap, sizeof(bitmap), &rendered_width, &rendered_height));
    previous_final = first_band_pixels();
    total_final = pixel_count();

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_text_renderer_render_caption(
            "previo\nactual", 0U, bitmap, sizeof(bitmap), &rendered_width, &rendered_height));
    previous_partial = first_band_pixels();
    total_partial = pixel_count();

    TEST_ASSERT_EQUAL_UINT32(previous_final, previous_partial);
    TEST_ASSERT_TRUE(total_partial < total_final);
    TEST_ASSERT_TRUE(total_partial > previous_partial);
}
