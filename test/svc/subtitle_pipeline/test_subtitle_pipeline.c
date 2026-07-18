#include <string.h>

#include "unity.h"
#include "errorno.h"
#include "subtitle_pipeline.h"

#include "mock_hw_platform.h"
#include "mock_subtitle_bram.h"
#include "mock_subtitle_overlay.h"
#include "mock_subtitle_text_renderer.h"

static subtitle_pipeline_t pipeline;
static uint8_t bitmap[2];
static uint8_t captured_caption_is_final;

static void expect_init_success(void)
{
    hw_platform_init_ExpectAndReturn(0); // SRC-C02: acquire shared MMIO platform
    subtitle_overlay_init_ExpectAnyArgsAndReturn(0);
    subtitle_bram_init_ExpectAnyArgsAndReturn(0);
    subtitle_overlay_configure_ExpectAnyArgsAndReturn(0);
    subtitle_bram_clear_ExpectAnyArgsAndReturn(0);
    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0);
}

static int renderer_caption_stub(char const* text,
                                 uint8_t current_is_final,
                                 uint8_t* dst,
                                 size_t dst_size,
                                 uint32_t* width,
                                 uint32_t* height,
                                 int call_count)
{
    (void)text;
    (void)dst;
    (void)dst_size;
    (void)call_count;

    captured_caption_is_final = current_is_final;
    *width = SUBTITLE_BRAM_MASK_WIDTH;
    *height = SUBTITLE_BRAM_MASK_HEIGHT;
    return 0;
}

void setUp(void)
{
    memset(&pipeline, 0, sizeof(pipeline));
    memset(bitmap, 0xFF, sizeof(bitmap));
    captured_caption_is_final = 0U;
}

void tearDown(void)
{}

void test_subtitle_pipeline_init_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_init(NULL, 1280U, 720U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_init(&pipeline, 0U, 720U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_init(&pipeline, 1280U, 0U));
}

void test_subtitle_pipeline_init_configures_default_geometry_and_stays_disabled(void)
{
    expect_init_success();

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_init(&pipeline, 1280U, 720U));

    TEST_ASSERT_EQUAL_UINT8(1U, pipeline.initialized);
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.enabled);
    TEST_ASSERT_EQUAL_UINT32(1280U, pipeline.display_width);
    TEST_ASSERT_EQUAL_UINT32(720U, pipeline.display_height);
    TEST_ASSERT_EQUAL_UINT32(1024U, pipeline.config.width);
    TEST_ASSERT_EQUAL_UINT32(256U, pipeline.config.height);
    TEST_ASSERT_EQUAL_UINT32(128U, pipeline.config.x);
    TEST_ASSERT_EQUAL_UINT32(428U, pipeline.config.y);
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_PIPELINE_DEFAULT_BAR_COLOR, pipeline.config.bar_color);
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_PIPELINE_DEFAULT_TEXT_COLOR, pipeline.config.text_color);
}

void test_subtitle_pipeline_init_enforces_minimum_mask_sized_bar(void)
{
    expect_init_success();

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_init(&pipeline, 200U, 100U));

    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_BRAM_MASK_WIDTH, pipeline.config.width);
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_BRAM_MASK_HEIGHT, pipeline.config.height);
    TEST_ASSERT_EQUAL_UINT32(0U, pipeline.config.x);
    TEST_ASSERT_EQUAL_UINT32(0U, pipeline.config.y);
}

void test_subtitle_pipeline_init_returns_hal_errors(void)
{
    // SRC-C02: a HAL failure after acquiring the platform must release it again.
    hw_platform_init_ExpectAndReturn(0);
    subtitle_overlay_init_ExpectAnyArgsAndReturn(-EIO);
    hw_platform_cleanup_Expect();
    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_init(&pipeline, 1280U, 720U));
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.initialized);
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.platform_ready);

    hw_platform_init_ExpectAndReturn(0);
    subtitle_overlay_init_ExpectAnyArgsAndReturn(0);
    subtitle_bram_init_ExpectAnyArgsAndReturn(-EIO);
    hw_platform_cleanup_Expect();
    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_init(&pipeline, 1280U, 720U));
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.initialized);
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.platform_ready);
}

void test_subtitle_pipeline_init_returns_eio_when_platform_acquire_fails(void)
{
    // SRC-C02: if the shared platform cannot be acquired, no overlay/BRAM work
    // happens and no reference is held.
    hw_platform_init_ExpectAndReturn(-1);
    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_init(&pipeline, 1280U, 720U));
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.initialized);
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.platform_ready);
}

void test_subtitle_pipeline_cleanup_disables_initialized_overlay_and_resets_state(void)
{
    pipeline.initialized = 1U;
    pipeline.enabled = 1U;
    pipeline.platform_ready = 1U;

    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0);
    hw_platform_cleanup_Expect(); // SRC-C02: release the platform reference

    subtitle_pipeline_cleanup(&pipeline);

    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.initialized);
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.enabled);
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.platform_ready);
}

void test_subtitle_pipeline_cleanup_releases_platform_even_when_not_initialized(void)
{
    // SRC-C02: a pipeline that acquired the platform but failed before becoming
    // fully initialized must still release its reference on cleanup.
    pipeline.initialized = 0U;
    pipeline.platform_ready = 1U;

    hw_platform_cleanup_Expect();

    subtitle_pipeline_cleanup(&pipeline);

    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.platform_ready);
}

void test_subtitle_pipeline_cleanup_ignores_null_or_uninitialized_pipeline(void)
{
    subtitle_pipeline_cleanup(NULL);

    // No platform reference held: cleanup must not touch hw_platform.
    pipeline.initialized = 0U;
    pipeline.platform_ready = 0U;
    subtitle_pipeline_cleanup(&pipeline);
}

void test_subtitle_pipeline_clear_requires_initialized_pipeline_and_delegates_to_bram(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_clear(NULL));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, subtitle_pipeline_clear(&pipeline));

    pipeline.initialized = 1U;
    subtitle_bram_clear_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_clear(&pipeline));
}

void test_subtitle_pipeline_write_bitmap_requires_initialized_pipeline_and_delegates_to_bram(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          subtitle_pipeline_write_bitmap(NULL, bitmap, sizeof(bitmap), 1, 2, 3, 4));
    TEST_ASSERT_EQUAL_INT(
        -APP_ESTATE,
        subtitle_pipeline_write_bitmap(&pipeline, bitmap, sizeof(bitmap), 1, 2, 3, 4));

    pipeline.initialized = 1U;
    subtitle_bram_write_bitmap_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(
        0,
        subtitle_pipeline_write_bitmap(&pipeline, bitmap, sizeof(bitmap), 1, 2, 3, 4));
}

void test_subtitle_pipeline_write_caption_passes_current_final_state_to_renderer(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_write_caption(NULL, "hola", 0U));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, subtitle_pipeline_write_caption(&pipeline, "hola", 0U));

    pipeline.initialized = 1U;
    subtitle_text_renderer_render_caption_Stub(renderer_caption_stub);
    subtitle_bram_write_bitmap_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_write_caption(&pipeline, "hola", 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, captured_caption_is_final);
}

void test_subtitle_pipeline_write_text_treats_text_as_final_caption(void)
{
    pipeline.initialized = 1U;
    subtitle_text_renderer_render_caption_Stub(renderer_caption_stub);
    subtitle_bram_write_bitmap_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_write_text(&pipeline, "manual"));
    TEST_ASSERT_EQUAL_UINT8(1U, captured_caption_is_final);
}

void test_subtitle_pipeline_commit_clears_and_waits_for_sof(void)
{
    pipeline.initialized = 1U;

    subtitle_overlay_clear_sof_ExpectAnyArgsAndReturn(0);
    subtitle_overlay_wait_sof_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_commit(&pipeline));
    TEST_ASSERT_EQUAL_UINT8(1U, pipeline.initialized);
}

void test_subtitle_pipeline_commit_returns_timeout_without_resetting_pipeline(void)
{
    pipeline.initialized = 1U;

    subtitle_overlay_clear_sof_ExpectAnyArgsAndReturn(0);
    subtitle_overlay_wait_sof_ExpectAnyArgsAndReturn(-EAGAIN);

    TEST_ASSERT_EQUAL_INT(-EAGAIN, subtitle_pipeline_commit(&pipeline));
    TEST_ASSERT_EQUAL_UINT8(1U, pipeline.initialized);
}

void test_subtitle_pipeline_commit_returns_overlay_error(void)
{
    pipeline.initialized = 1U;

    subtitle_overlay_clear_sof_ExpectAnyArgsAndReturn(-EIO);

    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_commit(&pipeline));
    TEST_ASSERT_EQUAL_UINT8(1U, pipeline.initialized);
}

void test_subtitle_pipeline_nonblocking_sof_helpers_delegate_to_overlay(void)
{
    uint8_t sof_seen = 0U;
    uint32_t control = SUBTITLE_OVERLAY_CTRL_SOF;

    pipeline.initialized = 1U;

    subtitle_overlay_clear_sof_ExpectAnyArgsAndReturn(0);
    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_clear_sof(&pipeline));

    subtitle_overlay_read_control_ExpectAnyArgsAndReturn(0);
    subtitle_overlay_read_control_ReturnThruPtr_control(&control);
    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_poll_sof(&pipeline, &sof_seen));
    TEST_ASSERT_EQUAL_UINT8(1U, sof_seen);
}

void test_subtitle_pipeline_enable_updates_enabled_flag(void)
{
    pipeline.initialized = 1U;

    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0);
    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_enable(&pipeline, 1));
    TEST_ASSERT_EQUAL_UINT8(1U, pipeline.enabled);

    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0);
    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_enable(&pipeline, 0));
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.enabled);
}

void test_subtitle_pipeline_enable_rejects_uninitialized_or_returns_hal_failure(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_enable(NULL, 1));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, subtitle_pipeline_enable(&pipeline, 1));

    pipeline.initialized = 1U;
    subtitle_overlay_enable_ExpectAnyArgsAndReturn(-EIO);

    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_enable(&pipeline, 1));
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.enabled);
}
