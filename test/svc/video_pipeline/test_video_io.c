#include <string.h>

#include "unity.h"
#include "video_io.h"
#include "xparameters.h"
#include "xstatus.h"

#include "mock_video_dma.h"
#include "mock_video_dynclk.h"
#include "mock_video_gpio.h"
#include "mock_video_vtc.h"

static video_dma_t dma;
static video_input_t input;
static video_output_t output;
static video_pipeline_mode_t mode_720p;

static void init_mode(video_pipeline_mode_t* const mode, uint32_t width, uint32_t height)
{
    memset(mode, 0, sizeof(*mode));
    mode->timing.width = width;
    mode->timing.height = height;
    mode->timing.pixel_clock_mhz = 74.25;
}

void setUp(void)
{
    memset(&dma, 0, sizeof(dma));
    memset(&input, 0, sizeof(input));
    memset(&output, 0, sizeof(output));
    init_mode(&mode_720p, 1280U, 720U);
}

void tearDown(void)
{
}

void test_video_input_init_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_init(NULL, &dma, 5760U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_init(&input, NULL, 5760U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_init(&input, &dma, 0U));
}

void test_video_input_init_sets_dma_stride_and_initializes_hal(void)
{
    video_gpio_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_init_ExpectAnyArgsAndReturn(XST_SUCCESS);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_input_init(&input, &dma, 5760U));
    TEST_ASSERT_EQUAL_PTR(&dma, input.dma);
    TEST_ASSERT_EQUAL_UINT32(5760U, input.stride);
    TEST_ASSERT_EQUAL_INT(0, input.running);
    TEST_ASSERT_EQUAL_INT(0, input.detector_started);
}

void test_video_input_init_returns_gpio_error(void)
{
    video_gpio_init_ExpectAnyArgsAndReturn(XST_DEVICE_NOT_FOUND);

    TEST_ASSERT_EQUAL_INT(XST_DEVICE_NOT_FOUND, video_input_init(&input, &dma, 5760U));
}

void test_video_input_init_returns_vtc_error(void)
{
    video_gpio_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_init_ExpectAnyArgsAndReturn(XST_DEVICE_NOT_FOUND);

    TEST_ASSERT_EQUAL_INT(XST_DEVICE_NOT_FOUND, video_input_init(&input, &dma, 5760U));
}

void test_video_input_locked_returns_zero_for_null_or_gpio_lock_state(void)
{
    TEST_ASSERT_EQUAL_INT(0, video_input_locked(NULL));

    video_gpio_is_locked_ExpectAnyArgsAndReturn(1);
    TEST_ASSERT_EQUAL_INT(1, video_input_locked(&input));

    video_gpio_is_locked_ExpectAnyArgsAndReturn(0);
    TEST_ASSERT_EQUAL_INT(0, video_input_locked(&input));
}

void test_video_input_start_detector_rejects_null_and_is_idempotent(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_start_detector(NULL, 100U));

    input.detector_started = 1;
    input.detector_started_ms = 50U;
    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_input_start_detector(&input, 100U));
    TEST_ASSERT_EQUAL_UINT32(50U, input.detector_started_ms);
}

void test_video_input_start_detector_stores_start_time_on_success(void)
{
    video_vtc_start_detector_ExpectAnyArgsAndReturn(XST_SUCCESS);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_input_start_detector(&input, 1234U));
    TEST_ASSERT_EQUAL_INT(1, input.detector_started);
    TEST_ASSERT_EQUAL_UINT32(1234U, input.detector_started_ms);
}

void test_video_input_start_detector_returns_hal_error_without_marking_started(void)
{
    video_vtc_start_detector_ExpectAnyArgsAndReturn(XST_FAILURE);

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_input_start_detector(&input, 1234U));
    TEST_ASSERT_EQUAL_INT(0, input.detector_started);
}

void test_video_input_read_timing_rejects_invalid_arguments(void)
{
    video_vtc_timing_t timing;

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_read_timing(NULL, &timing));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_read_timing(&input, NULL));
}

void test_video_input_read_timing_caches_timing_on_success(void)
{
    video_vtc_timing_t timing = {0};

    timing.width = 1280U;
    timing.height = 720U;

    video_vtc_read_detector_timing_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_read_detector_timing_ReturnThruPtr_timing(&timing);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_input_read_timing(&input, &timing));
    TEST_ASSERT_EQUAL_UINT32(1280U, input.timing.width);
    TEST_ASSERT_EQUAL_UINT32(720U, input.timing.height);
}

void test_video_input_read_timing_does_not_cache_timing_on_no_data(void)
{
    video_vtc_timing_t timing = {0};

    input.timing.width = 800U;
    input.timing.height = 600U;
    timing.width = 1280U;
    timing.height = 720U;

    video_vtc_read_detector_timing_ExpectAnyArgsAndReturn(XST_NO_DATA);
    video_vtc_read_detector_timing_ReturnThruPtr_timing(&timing);

    TEST_ASSERT_EQUAL_INT(XST_NO_DATA, video_input_read_timing(&input, &timing));
    TEST_ASSERT_EQUAL_UINT32(800U, input.timing.width);
    TEST_ASSERT_EQUAL_UINT32(600U, input.timing.height);
}

void test_video_input_start_capture_rejects_invalid_arguments(void)
{
    input.dma = &dma;

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_start_capture(NULL, &mode_720p, 0U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_start_capture(&input, NULL, 0U));

    input.dma = NULL;
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_start_capture(&input, &mode_720p, 0U));
}

void test_video_input_start_capture_stops_existing_capture_then_starts_s2mm_dma(void)
{
    input.dma = &dma;
    input.stride = 5760U;
    input.running = 1;

    video_dma_stop_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_S2MM, XST_SUCCESS);
    video_dma_configure_ExpectAndReturn(&dma,
                                        VIDEO_DMA_CHANNEL_S2MM,
                                        mode_720p.timing.width,
                                        mode_720p.timing.height,
                                        input.stride,
                                        2U,
                                        XST_SUCCESS);
    video_dma_start_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_S2MM, XST_SUCCESS);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_input_start_capture(&input, &mode_720p, 2U));
    TEST_ASSERT_EQUAL_INT(1, input.running);
    TEST_ASSERT_EQUAL_UINT32(2U, input.frame_index);
}

void test_video_input_start_capture_returns_configure_error(void)
{
    input.dma = &dma;
    input.stride = 5760U;

    video_dma_configure_ExpectAndReturn(&dma,
                                        VIDEO_DMA_CHANNEL_S2MM,
                                        mode_720p.timing.width,
                                        mode_720p.timing.height,
                                        input.stride,
                                        1U,
                                        XST_DMA_ERROR);

    TEST_ASSERT_EQUAL_INT(XST_DMA_ERROR, video_input_start_capture(&input, &mode_720p, 1U));
    TEST_ASSERT_EQUAL_INT(0, input.running);
}

void test_video_input_start_capture_returns_start_error(void)
{
    input.dma = &dma;
    input.stride = 5760U;

    video_dma_configure_ExpectAndReturn(&dma,
                                        VIDEO_DMA_CHANNEL_S2MM,
                                        mode_720p.timing.width,
                                        mode_720p.timing.height,
                                        input.stride,
                                        1U,
                                        XST_SUCCESS);
    video_dma_start_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_S2MM, XST_DMA_ERROR);

    TEST_ASSERT_EQUAL_INT(XST_DMA_ERROR, video_input_start_capture(&input, &mode_720p, 1U));
    TEST_ASSERT_EQUAL_INT(0, input.running);
}

void test_video_input_stop_rejects_null_and_resets_state(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_input_stop(NULL));

    input.dma = &dma;
    input.running = 1;
    input.detector_started = 1;
    input.timing.width = 1280U;

    video_dma_stop_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_S2MM, XST_SUCCESS);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_input_stop(&input));
    TEST_ASSERT_EQUAL_INT(0, input.running);
    TEST_ASSERT_EQUAL_INT(0, input.detector_started);
    TEST_ASSERT_EQUAL_UINT32(0U, input.timing.width);
}

void test_video_input_stop_allows_null_dma(void)
{
    input.dma = NULL;
    input.running = 1;
    input.detector_started = 1;

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_input_stop(&input));
    TEST_ASSERT_EQUAL_INT(0, input.running);
    TEST_ASSERT_EQUAL_INT(0, input.detector_started);
}

void test_video_output_init_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_output_init(NULL, &dma, 5760U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_output_init(&output, NULL, 5760U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_output_init(&output, &dma, 0U));
}

void test_video_output_init_sets_dma_stride_and_initializes_hal(void)
{
    video_dynclk_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_init_ExpectAnyArgsAndReturn(XST_SUCCESS);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_output_init(&output, &dma, 5760U));
    TEST_ASSERT_EQUAL_PTR(&dma, output.dma);
    TEST_ASSERT_EQUAL_UINT32(5760U, output.stride);
    TEST_ASSERT_EQUAL_INT(0, output.running);
}

void test_video_output_init_returns_dynclk_error(void)
{
    video_dynclk_init_ExpectAnyArgsAndReturn(XST_DEVICE_NOT_FOUND);

    TEST_ASSERT_EQUAL_INT(XST_DEVICE_NOT_FOUND, video_output_init(&output, &dma, 5760U));
}

void test_video_output_init_returns_vtc_error(void)
{
    video_dynclk_init_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_init_ExpectAnyArgsAndReturn(XST_DEVICE_NOT_FOUND);

    TEST_ASSERT_EQUAL_INT(XST_DEVICE_NOT_FOUND, video_output_init(&output, &dma, 5760U));
}

void test_video_output_start_rejects_invalid_arguments(void)
{
    output.dma = &dma;

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_output_start(NULL, &mode_720p, 0U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_output_start(&output, NULL, 0U));

    output.dma = NULL;
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_output_start(&output, &mode_720p, 0U));
}

void test_video_output_start_stops_existing_output_then_starts_mm2s_dma(void)
{
    output.dma = &dma;
    output.stride = 5760U;
    output.running = 1;

    video_vtc_stop_generator_ExpectAnyArgs();
    video_dma_stop_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_MM2S, XST_SUCCESS);
    video_dynclk_configure_ExpectAndReturn(&output.dynclk, mode_720p.timing.pixel_clock_mhz, XST_SUCCESS);
    video_vtc_configure_generator_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_start_generator_ExpectAnyArgs();
    video_dma_configure_ExpectAndReturn(&dma,
                                        VIDEO_DMA_CHANNEL_MM2S,
                                        mode_720p.timing.width,
                                        mode_720p.timing.height,
                                        output.stride,
                                        2U,
                                        XST_SUCCESS);
    video_dma_start_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_MM2S, XST_SUCCESS);
    video_dma_select_frame_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_MM2S, 2U, XST_SUCCESS);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_output_start(&output, &mode_720p, 2U));
    TEST_ASSERT_EQUAL_INT(1, output.running);
    TEST_ASSERT_EQUAL_PTR(&mode_720p, output.mode);
    TEST_ASSERT_EQUAL_UINT32(2U, output.frame_index);
}

void test_video_output_start_returns_dynclk_error(void)
{
    output.dma = &dma;

    video_dynclk_configure_ExpectAndReturn(&output.dynclk, mode_720p.timing.pixel_clock_mhz, XST_FAILURE);

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_output_start(&output, &mode_720p, 0U));
    TEST_ASSERT_EQUAL_INT(0, output.running);
}

void test_video_output_start_returns_vtc_configure_error(void)
{
    output.dma = &dma;

    video_dynclk_configure_ExpectAndReturn(&output.dynclk, mode_720p.timing.pixel_clock_mhz, XST_SUCCESS);
    video_vtc_configure_generator_ExpectAnyArgsAndReturn(XST_FAILURE);

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_output_start(&output, &mode_720p, 0U));
    TEST_ASSERT_EQUAL_INT(0, output.running);
}

void test_video_output_start_returns_dma_configure_error_after_starting_generator(void)
{
    output.dma = &dma;
    output.stride = 5760U;

    video_dynclk_configure_ExpectAndReturn(&output.dynclk, mode_720p.timing.pixel_clock_mhz, XST_SUCCESS);
    video_vtc_configure_generator_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_start_generator_ExpectAnyArgs();
    video_dma_configure_ExpectAndReturn(&dma,
                                        VIDEO_DMA_CHANNEL_MM2S,
                                        mode_720p.timing.width,
                                        mode_720p.timing.height,
                                        output.stride,
                                        0U,
                                        XST_DMA_ERROR);

    TEST_ASSERT_EQUAL_INT(XST_DMA_ERROR, video_output_start(&output, &mode_720p, 0U));
    TEST_ASSERT_EQUAL_INT(0, output.running);
}

void test_video_output_start_returns_dma_start_error(void)
{
    output.dma = &dma;
    output.stride = 5760U;

    video_dynclk_configure_ExpectAndReturn(&output.dynclk, mode_720p.timing.pixel_clock_mhz, XST_SUCCESS);
    video_vtc_configure_generator_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_start_generator_ExpectAnyArgs();
    video_dma_configure_ExpectAndReturn(&dma,
                                        VIDEO_DMA_CHANNEL_MM2S,
                                        mode_720p.timing.width,
                                        mode_720p.timing.height,
                                        output.stride,
                                        0U,
                                        XST_SUCCESS);
    video_dma_start_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_MM2S, XST_DMA_ERROR);

    TEST_ASSERT_EQUAL_INT(XST_DMA_ERROR, video_output_start(&output, &mode_720p, 0U));
    TEST_ASSERT_EQUAL_INT(0, output.running);
}

void test_video_output_start_returns_select_frame_error(void)
{
    output.dma = &dma;
    output.stride = 5760U;

    video_dynclk_configure_ExpectAndReturn(&output.dynclk, mode_720p.timing.pixel_clock_mhz, XST_SUCCESS);
    video_vtc_configure_generator_ExpectAnyArgsAndReturn(XST_SUCCESS);
    video_vtc_start_generator_ExpectAnyArgs();
    video_dma_configure_ExpectAndReturn(&dma,
                                        VIDEO_DMA_CHANNEL_MM2S,
                                        mode_720p.timing.width,
                                        mode_720p.timing.height,
                                        output.stride,
                                        0U,
                                        XST_SUCCESS);
    video_dma_start_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_MM2S, XST_SUCCESS);
    video_dma_select_frame_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_MM2S, 0U, XST_DMA_ERROR);

    TEST_ASSERT_EQUAL_INT(XST_DMA_ERROR, video_output_start(&output, &mode_720p, 0U));
    TEST_ASSERT_EQUAL_INT(0, output.running);
}

void test_video_output_stop_rejects_null_and_resets_state(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_output_stop(NULL));

    output.dma = &dma;
    output.running = 1;
    output.mode = &mode_720p;

    video_vtc_stop_generator_ExpectAnyArgs();
    video_dma_stop_ExpectAndReturn(&dma, VIDEO_DMA_CHANNEL_MM2S, XST_SUCCESS);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_output_stop(&output));
    TEST_ASSERT_EQUAL_INT(0, output.running);
    TEST_ASSERT_NULL(output.mode);
}

void test_video_output_stop_allows_null_dma(void)
{
    output.dma = NULL;
    output.running = 1;
    output.mode = &mode_720p;

    video_vtc_stop_generator_ExpectAnyArgs();

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_output_stop(&output));
    TEST_ASSERT_EQUAL_INT(0, output.running);
    TEST_ASSERT_NULL(output.mode);
}
