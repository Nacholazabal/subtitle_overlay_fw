#include "unity.h"
#include "subtitle_font.h"

void setUp(void)
{}

void tearDown(void)
{}

void test_subtitle_font_exposes_lato_34_pixel_line_metrics(void)
{
    TEST_ASSERT_EQUAL_UINT32(34U, SUBTITLE_FONT_PIXEL_SIZE);
    TEST_ASSERT_EQUAL_UINT32(41U, subtitle_font_line_height());
}

void test_subtitle_font_uses_proportional_horizontal_metrics(void)
{
    subtitle_font_glyph_t const* const narrow = subtitle_font_lookup((uint32_t)'i');
    subtitle_font_glyph_t const* const wide = subtitle_font_lookup((uint32_t)'m');

    TEST_ASSERT_TRUE(narrow->advance < wide->advance);
}

void test_subtitle_font_contains_real_spanish_glyphs(void)
{
    subtitle_font_glyph_t const* const enie = subtitle_font_lookup(0x00F1U);
    subtitle_font_glyph_t const* const accented = subtitle_font_lookup(0x00F3U);
    subtitle_font_glyph_t const* const inverted = subtitle_font_lookup(0x00BFU);

    TEST_ASSERT_EQUAL_UINT32(0x00F1U, enie->codepoint);
    TEST_ASSERT_EQUAL_UINT32(0x00F3U, accented->codepoint);
    TEST_ASSERT_EQUAL_UINT32(0x00BFU, inverted->codepoint);
    TEST_ASSERT_NOT_NULL(enie->bitmap);
    TEST_ASSERT_NOT_NULL(accented->bitmap);
    TEST_ASSERT_NOT_NULL(inverted->bitmap);
}

void test_subtitle_font_metrics_leave_spacing_between_consecutive_c_glyphs(void)
{
    subtitle_font_glyph_t const* const glyph = subtitle_font_lookup((uint32_t)'c');

    TEST_ASSERT_LESS_OR_EQUAL_UINT16(glyph->advance, (uint16_t)(glyph->x_offset + glyph->width));
}

void test_subtitle_font_falls_back_to_question_mark(void)
{
    subtitle_font_glyph_t const* const glyph = subtitle_font_lookup(0x20ACU);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)'?', glyph->codepoint);
}
