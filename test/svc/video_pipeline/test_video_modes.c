#include <string.h>

#include "unity.h"
#include "video_modes.h"

void setUp(void)
{}

void tearDown(void)
{}

void test_video_modes_default_returns_first_supported_mode(void)
{
    size_t count = 0U;
    video_pipeline_mode_t const* const all_modes = video_modes_all(&count);
    video_pipeline_mode_t const* const default_mode = video_modes_default();

    TEST_ASSERT_NOT_NULL(all_modes);
    TEST_ASSERT_NOT_NULL(default_mode);
    TEST_ASSERT_EQUAL_PTR(all_modes, default_mode);
    TEST_ASSERT_GREATER_THAN_UINT(0U, count);
    TEST_ASSERT_EQUAL_UINT32(640U, default_mode->timing.width);
    TEST_ASSERT_EQUAL_UINT32(480U, default_mode->timing.height);
    TEST_ASSERT_EQUAL_STRING("640x480@60Hz", default_mode->label);
}

void test_video_modes_find_returns_supported_modes_by_resolution(void)
{
    video_pipeline_mode_t const* mode;

    mode = video_modes_find(640U, 480U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("640x480@60Hz", mode->label);

    mode = video_modes_find(800U, 600U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("800x600@60Hz", mode->label);

    mode = video_modes_find(1280U, 720U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("1280x720@60Hz", mode->label);

    mode = video_modes_find(1280U, 1024U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("1280x1024@60Hz", mode->label);

    mode = video_modes_find(1920U, 1080U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("1920x1080@60Hz", mode->label);
}

void test_video_modes_find_rejects_unsupported_resolution(void)
{
    TEST_ASSERT_NULL(video_modes_find(1024U, 768U));
    TEST_ASSERT_NULL(video_modes_find(1920U, 1200U));
    TEST_ASSERT_NULL(video_modes_find(0U, 0U));
}

void test_video_modes_all_reports_mode_table_and_accepts_null_count(void)
{
    size_t count = 0U;
    video_pipeline_mode_t const* const modes = video_modes_all(&count);

    TEST_ASSERT_NOT_NULL(modes);
    TEST_ASSERT_EQUAL_UINT(5U, count);
    TEST_ASSERT_EQUAL_STRING("640x480@60Hz", modes[0].label);
    TEST_ASSERT_EQUAL_STRING("1920x1080@60Hz", modes[count - 1U].label);
    TEST_ASSERT_EQUAL_PTR(modes, video_modes_all(NULL));
}
