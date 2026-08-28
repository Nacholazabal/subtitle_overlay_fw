#include <string.h>

#include "unity.h"
#include "errorno.h"
#include "subtitle_pipeline.h"

#include "mock_hw_platform.h"
#include "mock_subtitle_bram.h"
#include "mock_subtitle_overlay.h"
#include "mock_subtitle_text_renderer.h"

static subtitle_pipeline_t pipeline;
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
    *width = 640U;
    *height = 50U;
    return 0;
}

void setUp(void)
{
    memset(&pipeline, 0, sizeof(pipeline));
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
    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0); // SRC-M03 rollback disable
    hw_platform_cleanup_Expect();
    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_init(&pipeline, 1280U, 720U));
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.initialized);
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.platform_ready);
}

void test_subtitle_pipeline_init_disables_overlay_when_a_late_stage_fails(void)
{
    // SRC-M03: a failure after the overlay is up (here at bram_clear) must leave
    // the overlay disabled and release the platform, not partially configured.
    hw_platform_init_ExpectAndReturn(0);
    subtitle_overlay_init_ExpectAnyArgsAndReturn(0);
    subtitle_bram_init_ExpectAnyArgsAndReturn(0);
    subtitle_overlay_configure_ExpectAnyArgsAndReturn(0);
    subtitle_bram_clear_ExpectAnyArgsAndReturn(-EIO);
    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0); // rollback disable
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
    pipeline.platform_ready = 1U;

    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0);
    hw_platform_cleanup_Expect(); // SRC-C02: release the platform reference

    subtitle_pipeline_cleanup(&pipeline);

    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.initialized);
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

void test_subtitle_pipeline_set_box_centers_compact_geometry_above_bottom_margin(void)
{
    pipeline.initialized = 1U;
    pipeline.display_width = 1280U;
    pipeline.display_height = 720U;
    pipeline.config.bar_color = SUBTITLE_PIPELINE_DEFAULT_BAR_COLOR;
    pipeline.config.text_color = SUBTITLE_PIPELINE_DEFAULT_TEXT_COLOR;
    subtitle_overlay_configure_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_set_box(&pipeline, 640U, 50U));

    TEST_ASSERT_EQUAL_UINT32(640U, pipeline.config.width);
    TEST_ASSERT_EQUAL_UINT32(50U, pipeline.config.height);
    TEST_ASSERT_EQUAL_UINT32(320U, pipeline.config.x);
    TEST_ASSERT_EQUAL_UINT32(634U, pipeline.config.y);
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_PIPELINE_DEFAULT_BAR_COLOR, pipeline.config.bar_color);
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_PIPELINE_DEFAULT_TEXT_COLOR, pipeline.config.text_color);
}

void test_subtitle_pipeline_set_box_rejects_invalid_geometry(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_set_box(NULL, 100U, 40U));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, subtitle_pipeline_set_box(&pipeline, 100U, 40U));

    pipeline.initialized = 1U;
    pipeline.display_width = 1280U;
    pipeline.display_height = 720U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_set_box(&pipeline, 0U, 40U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_set_box(&pipeline, 100U, 0U));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          subtitle_pipeline_set_box(&pipeline, SUBTITLE_BRAM_MASK_WIDTH + 1U, 40U));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        subtitle_pipeline_set_box(&pipeline, 100U, SUBTITLE_BRAM_MASK_HEIGHT + 1U));
}

void test_subtitle_pipeline_set_box_preserves_configuration_on_hal_failure(void)
{
    subtitle_overlay_config_t const previous = {
        .x = 128U,
        .y = 428U,
        .width = 1024U,
        .height = 256U,
        .bar_color = SUBTITLE_PIPELINE_DEFAULT_BAR_COLOR,
        .text_color = SUBTITLE_PIPELINE_DEFAULT_TEXT_COLOR,
    };

    pipeline.initialized = 1U;
    pipeline.display_width = 1280U;
    pipeline.display_height = 720U;
    pipeline.config = previous;
    subtitle_overlay_configure_ExpectAnyArgsAndReturn(-EIO);

    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_set_box(&pipeline, 640U, 50U));
    TEST_ASSERT_EQUAL_MEMORY(&previous, &pipeline.config, sizeof(previous));
}

void test_subtitle_pipeline_write_caption_passes_current_final_state_to_renderer(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_write_caption(NULL, "hola", 0U));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, subtitle_pipeline_write_caption(&pipeline, "hola", 0U));

    pipeline.initialized = 1U;
    pipeline.display_width = 1280U;
    pipeline.display_height = 720U;
    subtitle_text_renderer_render_caption_Stub(renderer_caption_stub);
    subtitle_overlay_configure_ExpectAnyArgsAndReturn(0);
    subtitle_bram_clear_ExpectAnyArgsAndReturn(0);
    subtitle_bram_write_bitmap_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_write_caption(&pipeline, "hola", 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, captured_caption_is_final);
}

void test_subtitle_pipeline_write_caption_renders_final_text_solid(void)
{
    pipeline.initialized = 1U;
    pipeline.display_width = 1280U;
    pipeline.display_height = 720U;
    subtitle_text_renderer_render_caption_Stub(renderer_caption_stub);
    subtitle_overlay_configure_ExpectAnyArgsAndReturn(0);
    subtitle_bram_clear_ExpectAnyArgsAndReturn(0);
    subtitle_bram_write_bitmap_ExpectAnyArgsAndReturn(0);

    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_write_caption(&pipeline, "manual", 1U));
    TEST_ASSERT_EQUAL_UINT8(1U, captured_caption_is_final);
}

void test_subtitle_pipeline_enable_delegates_to_overlay(void)
{
    pipeline.initialized = 1U;

    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0);
    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_enable(&pipeline, 1));

    subtitle_overlay_enable_ExpectAnyArgsAndReturn(0);
    TEST_ASSERT_EQUAL_INT(0, subtitle_pipeline_enable(&pipeline, 0));
}

void test_subtitle_pipeline_enable_rejects_uninitialized_or_returns_hal_failure(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_enable(NULL, 1));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, subtitle_pipeline_enable(&pipeline, 1));

    pipeline.initialized = 1U;
    subtitle_overlay_enable_ExpectAnyArgsAndReturn(-EIO);

    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_pipeline_enable(&pipeline, 1));
}

void test_subtitle_pipeline_write_caption_propagates_renderer_failure(void)
{
    pipeline.initialized = 1U;

    // A renderer failure aborts before any BRAM/overlay geometry work.
    subtitle_text_renderer_render_caption_ExpectAnyArgsAndReturn(-EINVAL);

    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_pipeline_write_caption(&pipeline, "hola", 1U));
}

