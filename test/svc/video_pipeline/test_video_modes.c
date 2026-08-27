#include <string.h>

#include "unity.h"
#include "video_modes.h"

void setUp(void)
{}

void tearDown(void)
{}

void test_video_modes_find_returns_supported_modes_by_resolution(void)
{
    video_pipeline_mode_t const* mode;

    mode = video_modes_find(640U, 480U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("640x480@60Hz", mode->label);
    TEST_ASSERT_EQUAL_UINT32(640U, mode->timing.width);
    TEST_ASSERT_EQUAL_UINT32(480U, mode->timing.height);

    mode = video_modes_find(800U, 600U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("800x600@60Hz", mode->label);

    mode = video_modes_find(1280U, 720U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("1280x720@60Hz", mode->label);
    TEST_ASSERT_EQUAL_FLOAT(74.25f, (float)mode->timing.pixel_clock_mhz);

    mode = video_modes_find(1280U, 1024U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("1280x1024@60Hz", mode->label);

    mode = video_modes_find(1920U, 1080U);
    TEST_ASSERT_NOT_NULL(mode);
    TEST_ASSERT_EQUAL_STRING("1920x1080@60Hz", mode->label);
    TEST_ASSERT_EQUAL_UINT32(1920U, mode->timing.width);
    TEST_ASSERT_EQUAL_UINT32(1080U, mode->timing.height);
}

void test_video_modes_find_returns_a_stable_pointer_into_the_static_table(void)
{
    TEST_ASSERT_EQUAL_PTR(video_modes_find(1280U, 720U), video_modes_find(1280U, 720U));
    TEST_ASSERT_NOT_EQUAL(video_modes_find(640U, 480U), video_modes_find(1920U, 1080U));
}

void test_video_modes_find_rejects_unsupported_resolution(void)
{
    TEST_ASSERT_NULL(video_modes_find(1024U, 768U));
    TEST_ASSERT_NULL(video_modes_find(1920U, 1200U));
    TEST_ASSERT_NULL(video_modes_find(0U, 0U));
    // A width/height pair that exists in the table, but not together.
    TEST_ASSERT_NULL(video_modes_find(1920U, 720U));
}
