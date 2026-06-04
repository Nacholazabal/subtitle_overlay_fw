#include <string.h>

#include "unity.h"
#include "errorno.h"
#include "subtitle_bram.h"

#include "mock_hw_platform.h"

static subtitle_bram_t bram;
static uint32_t bram_words[SUBTITLE_BRAM_WORD_COUNT];

void setUp(void)
{
    memset(&bram, 0, sizeof(bram));
    memset(bram_words, 0xA5, sizeof(bram_words));
    bram.base = (uintptr_t)bram_words;
}

void tearDown(void)
{
}

void test_subtitle_bram_init_uses_mapped_platform_region(void)
{
    memset(&bram, 0, sizeof(bram));
    hw_platform_base_ExpectAndReturn(HW_REGION_SUBTITLE_BRAM, (uintptr_t)bram_words);

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_init(&bram));
    TEST_ASSERT_EQUAL_UINT((uintptr_t)bram_words, bram.base);
}

void test_subtitle_bram_init_rejects_null_and_missing_region(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_init(NULL));

    hw_platform_base_ExpectAndReturn(HW_REGION_SUBTITLE_BRAM, (uintptr_t)0);
    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_bram_init(&bram));
}

void test_subtitle_bram_clear_zeros_all_words(void)
{
    uint32_t i;

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_clear(&bram));

    for (i = 0U; i < SUBTITLE_BRAM_WORD_COUNT; i++)
    {
        TEST_ASSERT_EQUAL_UINT32(0U, bram_words[i]);
    }
}

void test_subtitle_bram_set_and_clear_pixel_use_lsb_first_words(void)
{
    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_clear(&bram));

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0x00000001U, bram_words[0]);

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, 31, 0));
    TEST_ASSERT_EQUAL_UINT32(0x80000001U, bram_words[0]);

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, 32, 0));
    TEST_ASSERT_EQUAL_UINT32(0x00000001U, bram_words[1]);

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, 3, 1));
    TEST_ASSERT_EQUAL_UINT32(0x00000008U, bram_words[SUBTITLE_BRAM_WORDS_PER_ROW]);

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_clear_pixel(&bram, 31, 0));
    TEST_ASSERT_EQUAL_UINT32(0x00000001U, bram_words[0]);
}

void test_subtitle_bram_pixel_writes_ignore_out_of_range_coordinates(void)
{
    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_clear(&bram));

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, -1, 0));
    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, 0, -1));
    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, SUBTITLE_BRAM_MASK_WIDTH, 0));
    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_set_pixel(&bram, 0, SUBTITLE_BRAM_MASK_HEIGHT));

    TEST_ASSERT_EQUAL_UINT32(0U, bram_words[0]);
}

void test_subtitle_bram_write_bitmap_converts_msb_first_source_to_lsb_first_bram(void)
{
    uint8_t const bitmap[] = {
        0xA0U, /* 1010.... */
        0x40U, /* 0100.... */
    };

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_clear(&bram));
    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_write_bitmap(&bram, bitmap, 1, 2, 4, 2));

    TEST_ASSERT_EQUAL_UINT32(0x0000000AU, bram_words[(2U * SUBTITLE_BRAM_WORDS_PER_ROW)]);
    TEST_ASSERT_EQUAL_UINT32(0x00000004U, bram_words[(3U * SUBTITLE_BRAM_WORDS_PER_ROW)]);
}

void test_subtitle_bram_write_bitmap_clips_destination_and_clears_zero_source_bits(void)
{
    uint8_t const bitmap[] = {
        0x80U, /* Only the clipped-left source bit is set. */
        0x40U, /* Visible destination x=0 is set on second row. */
    };

    bram_words[0] = 0xFFFFFFFFU;
    bram_words[SUBTITLE_BRAM_WORDS_PER_ROW] = 0xFFFFFFFFU;

    TEST_ASSERT_EQUAL_INT(0, subtitle_bram_write_bitmap(&bram, bitmap, -1, 0, 3, 2));

    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFCU, bram_words[0]);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFDU, bram_words[SUBTITLE_BRAM_WORDS_PER_ROW]);
}

void test_subtitle_bram_rejects_invalid_arguments(void)
{
    uint8_t const bitmap[] = {0xFFU};

    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_clear(NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_set_pixel(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_clear_pixel(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_write_bitmap(NULL, bitmap, 0, 0, 1, 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_write_bitmap(&bram, NULL, 0, 0, 1, 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_write_bitmap(&bram, bitmap, 0, 0, 0, 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_bram_write_bitmap(&bram, bitmap, 0, 0, 1, 0));

    bram.base = (uintptr_t)0;
    TEST_ASSERT_EQUAL_INT(-ESTATE, subtitle_bram_clear(&bram));
}
