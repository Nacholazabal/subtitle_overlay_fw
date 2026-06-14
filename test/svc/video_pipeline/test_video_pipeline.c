#include <string.h>

#include "unity.h"
#include "video_pipeline.h"
#include "xstatus.h"

#include "mock_hw_platform.h"
#include "mock_video_dma.h"
#include "mock_video_io.h"
#include "mock_video_modes.h"

static video_pipeline_t pipeline;
static video_pipeline_mode_t mode_720p;

static void expect_transport_stop(void)
{
    video_input_stop_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_output_stop_ExpectAnyArgsAndReturn(XST_SUCCESS);
}

static void expect_pipeline_cleanup_after_init_failure(void)
{
    expect_transport_stop();
    video_dma_cleanup_ExpectAnyArgs();
    hw_platform_cleanup_Expect();
}

static void init_mode(video_pipeline_mode_t* const mode, uint32_t width, uint32_t height)
{
    memset(mode, 0, sizeof(*mode));
    mode->timing.width = width;
    mode->timing.height = height;
}

static void expect_init_success(void)
{
    hw_platform_init_ExpectAndReturn(0);
    video_dma_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_output_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
}

void setUp(void)
{
    memset(&pipeline, 0, sizeof(pipeline));
    init_mode(&mode_720p, 1280U, 720U);
}

void tearDown(void)
{}

void test_video_pipeline_init_rejects_null_pipeline(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_pipeline_init(NULL));
}

void test_video_pipeline_init_success_sets_waiting_for_signal(void)
{
    expect_init_success();

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_pipeline_init(&pipeline));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_WAITING_FOR_SIGNAL, video_pipeline_get_state(&pipeline));
    TEST_ASSERT_NULL(video_pipeline_get_active_mode(&pipeline));
    TEST_ASSERT_EQUAL_UINT32(0U, pipeline.active_frame);
    TEST_ASSERT_EQUAL_UINT8(1U, pipeline.platform_ready);
}

void test_video_pipeline_init_reports_platform_failure(void)
{
    hw_platform_init_ExpectAndReturn(-1);

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_pipeline_init(&pipeline));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_ERROR, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_init_cleans_up_after_dma_failure(void)
{
    hw_platform_init_ExpectAndReturn(0);
    video_dma_init_ExpectAnyArgsAndReturn(XST_DMA_ERROR);
    expect_pipeline_cleanup_after_init_failure();

    TEST_ASSERT_EQUAL_INT(XST_DMA_ERROR, video_pipeline_init(&pipeline));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_UNINITIALIZED, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_init_cleans_up_after_output_failure(void)
{
    hw_platform_init_ExpectAndReturn(0);
    video_dma_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_output_init_ExpectAnyArgsAndReturn(XST_DEVICE_NOT_FOUND);
    expect_pipeline_cleanup_after_init_failure();

    TEST_ASSERT_EQUAL_INT(XST_DEVICE_NOT_FOUND, video_pipeline_init(&pipeline));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_UNINITIALIZED, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_init_cleans_up_after_input_failure(void)
{
    hw_platform_init_ExpectAndReturn(0);
    video_dma_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_output_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_init_ExpectAnyArgsAndReturn(XST_DEVICE_NOT_FOUND);
    expect_pipeline_cleanup_after_init_failure();

    TEST_ASSERT_EQUAL_INT(XST_DEVICE_NOT_FOUND, video_pipeline_init(&pipeline));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_UNINITIALIZED, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_cleanup_ignores_null_pipeline(void)
{
    video_pipeline_cleanup(NULL);
}

void test_video_pipeline_cleanup_stops_transport_dma_and_platform(void)
{
    pipeline.platform_ready = 1U;
    pipeline.active_mode = &mode_720p;
    pipeline.input_timing.width = 1280U;
    pipeline.state = VIDEO_PIPELINE_STREAMING;

    expect_transport_stop();
    video_dma_cleanup_ExpectAnyArgs();
    hw_platform_cleanup_Expect();

    video_pipeline_cleanup(&pipeline);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_UNINITIALIZED, video_pipeline_get_state(&pipeline));
    TEST_ASSERT_NULL(video_pipeline_get_active_mode(&pipeline));
    TEST_ASSERT_EQUAL_UINT8(0U, pipeline.platform_ready);
}

void test_video_pipeline_poll_rejects_null_or_uninitialized_pipeline(void)
{
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_ERROR, video_pipeline_poll(NULL, 100U));

    pipeline.state = VIDEO_PIPELINE_UNINITIALIZED;
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_ERROR, video_pipeline_poll(&pipeline, 100U));
}

void test_video_pipeline_poll_keeps_waiting_when_input_is_unlocked(void)
{
    pipeline.state = VIDEO_PIPELINE_WAITING_FOR_SIGNAL;
    video_input_locked_ExpectAnyArgsAndReturn(0U);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_UNCHANGED, video_pipeline_poll(&pipeline, 100U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_WAITING_FOR_SIGNAL, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_poll_reports_signal_lost_and_stops_transport(void)
{
    pipeline.state = VIDEO_PIPELINE_STREAMING;
    pipeline.active_mode = &mode_720p;
    pipeline.input_timing.width = 1280U;

    video_input_locked_ExpectAnyArgsAndReturn(0U);
    expect_transport_stop();

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_SIGNAL_LOST, video_pipeline_poll(&pipeline, 100U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_WAITING_FOR_SIGNAL, video_pipeline_get_state(&pipeline));
    TEST_ASSERT_NULL(video_pipeline_get_active_mode(&pipeline));
    TEST_ASSERT_EQUAL_UINT32(0U, pipeline.input_timing.width);
}

void test_video_pipeline_poll_starts_detector_when_signal_is_detected(void)
{
    pipeline.state = VIDEO_PIPELINE_WAITING_FOR_SIGNAL;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_start_detector_ExpectAnyArgsAndReturn(XST_SUCCESS);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_SIGNAL_DETECTED, video_pipeline_poll(&pipeline, 250U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_ACQUIRING_TIMING, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_poll_enters_error_when_detector_start_fails(void)
{
    pipeline.state = VIDEO_PIPELINE_WAITING_FOR_SIGNAL;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_start_detector_ExpectAnyArgsAndReturn(XST_FAILURE);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_ERROR, video_pipeline_poll(&pipeline, 250U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_ERROR, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_poll_waits_when_timing_is_not_ready(void)
{
    pipeline.state = VIDEO_PIPELINE_ACQUIRING_TIMING;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_read_timing_ExpectAnyArgsAndReturn(XST_NO_DATA);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_UNCHANGED, video_pipeline_poll(&pipeline, 300U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_ACQUIRING_TIMING, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_poll_enters_error_when_read_timing_fails(void)
{
    pipeline.state = VIDEO_PIPELINE_ACQUIRING_TIMING;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_read_timing_ExpectAnyArgsAndReturn(XST_FAILURE);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_ERROR, video_pipeline_poll(&pipeline, 300U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_ERROR, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_poll_reports_unsupported_timing(void)
{
    video_vtc_timing_t timing = {0};

    timing.width = 1024U;
    timing.height = 768U;
    pipeline.state = VIDEO_PIPELINE_ACQUIRING_TIMING;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_read_timing_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_read_timing_ReturnThruPtr_timing(&timing);
    video_modes_find_ExpectAndReturn(timing.width, timing.height, NULL);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_UNSUPPORTED_INPUT, video_pipeline_poll(&pipeline, 300U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_UNSUPPORTED_INPUT, video_pipeline_get_state(&pipeline));
    TEST_ASSERT_EQUAL_UINT32(timing.width, pipeline.input_timing.width);
    TEST_ASSERT_NULL(video_pipeline_get_active_mode(&pipeline));
}

void test_video_pipeline_poll_starts_passthrough_for_supported_timing(void)
{
    video_vtc_timing_t timing = {0};

    timing.width = 1280U;
    timing.height = 720U;
    pipeline.state = VIDEO_PIPELINE_ACQUIRING_TIMING;
    pipeline.active_frame = 2U;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_read_timing_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_read_timing_ReturnThruPtr_timing(&timing);
    video_modes_find_ExpectAndReturn(timing.width, timing.height, &mode_720p);
    video_output_start_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_start_capture_ExpectAnyArgsAndReturn(XST_SUCCESS);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_STREAMING_STARTED, video_pipeline_poll(&pipeline, 300U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_STREAMING, video_pipeline_get_state(&pipeline));
    TEST_ASSERT_EQUAL_PTR(&mode_720p, video_pipeline_get_active_mode(&pipeline));
    TEST_ASSERT_EQUAL_UINT32(timing.width, pipeline.input_timing.width);
}

void test_video_pipeline_poll_enters_error_when_output_start_fails(void)
{
    video_vtc_timing_t timing = {0};

    timing.width = 1280U;
    timing.height = 720U;
    pipeline.state = VIDEO_PIPELINE_ACQUIRING_TIMING;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_read_timing_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_read_timing_ReturnThruPtr_timing(&timing);
    video_modes_find_ExpectAndReturn(timing.width, timing.height, &mode_720p);
    video_output_start_ExpectAnyArgsAndReturn(XST_FAILURE);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_ERROR, video_pipeline_poll(&pipeline, 300U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_ERROR, video_pipeline_get_state(&pipeline));
    TEST_ASSERT_NULL(video_pipeline_get_active_mode(&pipeline));
}

void test_video_pipeline_poll_stops_output_when_input_capture_fails(void)
{
    video_vtc_timing_t timing = {0};

    timing.width = 1280U;
    timing.height = 720U;
    pipeline.state = VIDEO_PIPELINE_ACQUIRING_TIMING;

    video_input_locked_ExpectAnyArgsAndReturn(1U);
    video_input_read_timing_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_read_timing_ReturnThruPtr_timing(&timing);
    video_modes_find_ExpectAndReturn(timing.width, timing.height, &mode_720p);
    video_output_start_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_input_start_capture_ExpectAnyArgsAndReturn(XST_FAILURE);
    video_output_stop_ExpectAnyArgsAndReturn(XST_SUCCESS);

    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_ERROR, video_pipeline_poll(&pipeline, 300U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_ERROR, video_pipeline_get_state(&pipeline));
    TEST_ASSERT_NULL(video_pipeline_get_active_mode(&pipeline));
}

void test_video_pipeline_poll_is_unchanged_while_streaming_or_unsupported_with_lock(void)
{
    pipeline.state = VIDEO_PIPELINE_STREAMING;
    video_input_locked_ExpectAnyArgsAndReturn(1U);
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_UNCHANGED, video_pipeline_poll(&pipeline, 400U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_STREAMING, video_pipeline_get_state(&pipeline));

    pipeline.state = VIDEO_PIPELINE_UNSUPPORTED_INPUT;
    video_input_locked_ExpectAnyArgsAndReturn(1U);
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_POLL_UNCHANGED, video_pipeline_poll(&pipeline, 500U));
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_UNSUPPORTED_INPUT, video_pipeline_get_state(&pipeline));
}

void test_video_pipeline_getters_handle_null_pipeline(void)
{
    TEST_ASSERT_EQUAL(VIDEO_PIPELINE_UNINITIALIZED, video_pipeline_get_state(NULL));
    TEST_ASSERT_NULL(video_pipeline_get_active_mode(NULL));
}
