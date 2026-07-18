/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file video_pipeline.c
/// @brief Video pipeline service implementation
///

// === Headers files inclusions ==================================================================================== //

#include "video_pipeline.h"

#include <string.h>

#include "hw_platform.h"
#include "xstatus.h"

// === Macros definitions ========================================================================================== //

// SRC-H03: how long to wait for the input timing detector to report a mode
// before restarting it. Real sources deliver timing within tens of ms; this
// generous ceiling only trips on a stalled/absent detector.
#define VIDEO_PIPELINE_TIMING_TIMEOUT_MS (2000U)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void stop_transport(video_pipeline_t* pipeline);
static video_pipeline_poll_result_e start_passthrough(video_pipeline_t* pipeline,
                                                      video_pipeline_mode_t const* mode);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Stop both capture and display sides and clear active timing state.
 * @param pipeline Pipeline instance to stop.
 * @return None.
 */
static void stop_transport(video_pipeline_t* const pipeline)
{
    (void)video_input_stop(&pipeline->input);
    (void)video_output_stop(&pipeline->output);
    pipeline->active_mode = NULL;
    memset(&pipeline->input_timing, 0, sizeof(pipeline->input_timing));
}

/**
 * @brief Start mirrored input-to-output transport for a supported mode.
 * @param pipeline Initialized pipeline instance.
 * @param mode Supported mode matching the detected input.
 * @return Poll result indicating streaming start or an error.
 */
static video_pipeline_poll_result_e start_passthrough(video_pipeline_t* const pipeline,
                                                      video_pipeline_mode_t const* const mode)
{
    int status;

    // SRC-H02: single-buffer passthrough — capture and display share one frame.
    status = video_output_start(&pipeline->output, mode, VIDEO_PIPELINE_PASSTHROUGH_FRAME);
    if (status != XST_SUCCESS)
    {
        pipeline->state = VIDEO_PIPELINE_ERROR;
        return VIDEO_PIPELINE_POLL_ERROR;
    }

    status = video_input_start_capture(&pipeline->input, mode, VIDEO_PIPELINE_PASSTHROUGH_FRAME);
    if (status != XST_SUCCESS)
    {
        (void)video_output_stop(&pipeline->output);
        pipeline->state = VIDEO_PIPELINE_ERROR;
        return VIDEO_PIPELINE_POLL_ERROR;
    }

    pipeline->active_mode = mode;
    pipeline->state = VIDEO_PIPELINE_STREAMING;
    return VIDEO_PIPELINE_POLL_STREAMING_STARTED;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize platform mapping, shared DMA, and input/output helpers.
 * @param pipeline Pipeline instance to initialize.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or a lower-layer error code on failure.
 */
int video_pipeline_init(video_pipeline_t* const pipeline)
{
    int status;

    if (pipeline == NULL)
    {
        return XST_INVALID_PARAM;
    }

    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->state = VIDEO_PIPELINE_UNINITIALIZED;

    if (hw_platform_init() != 0)
    {
        pipeline->state = VIDEO_PIPELINE_ERROR;
        return XST_FAILURE;
    }
    pipeline->platform_ready = 1U;

    status = video_dma_init(&pipeline->dma, pipeline->frames, VIDEO_PIPELINE_FRAME_COUNT);
    if (status != XST_SUCCESS)
    {
        video_pipeline_cleanup(pipeline);
        return status;
    }

    status = video_output_init(&pipeline->output, &pipeline->dma, VIDEO_PIPELINE_STRIDE);
    if (status != XST_SUCCESS)
    {
        video_pipeline_cleanup(pipeline);
        return status;
    }

    status = video_input_init(&pipeline->input, &pipeline->dma, VIDEO_PIPELINE_STRIDE);
    if (status != XST_SUCCESS)
    {
        video_pipeline_cleanup(pipeline);
        return status;
    }

    pipeline->state = VIDEO_PIPELINE_WAITING_FOR_SIGNAL;
    return XST_SUCCESS;
}

/**
 * @brief Stop the pipeline and release DMA/platform resources.
 * @param pipeline Pipeline instance to clean up.
 * @return None.
 */
void video_pipeline_cleanup(video_pipeline_t* const pipeline)
{
    if (pipeline == NULL)
    {
        return;
    }

    stop_transport(pipeline);
    video_dma_cleanup(&pipeline->dma);

    if (pipeline->platform_ready)
    {
        hw_platform_cleanup();
    }

    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->state = VIDEO_PIPELINE_UNINITIALIZED;
}

/**
 * @brief Advance the video pipeline state machine by one nonblocking step.
 * @param pipeline Initialized pipeline instance.
 * @param now_ms Current monotonic time in milliseconds.
 * @return Poll result describing any state change or error observed this step.
 */
video_pipeline_poll_result_e video_pipeline_poll(video_pipeline_t* const pipeline, uint32_t now_ms)
{
    video_vtc_timing_t timing;
    video_pipeline_mode_t const* mode;
    video_pipeline_poll_result_e result;
    int status = 0;
    uint8_t locked;

    if ((pipeline == NULL) || (pipeline->state == VIDEO_PIPELINE_UNINITIALIZED))
    {
        return VIDEO_PIPELINE_POLL_ERROR;
    }

    locked = video_input_locked(&pipeline->input);
    if (!locked && (pipeline->state == VIDEO_PIPELINE_WAITING_FOR_SIGNAL))
    {
        return VIDEO_PIPELINE_POLL_UNCHANGED;
    }

    if (!locked)
    {
        stop_transport(pipeline);
        pipeline->state = VIDEO_PIPELINE_WAITING_FOR_SIGNAL;
        return VIDEO_PIPELINE_POLL_SIGNAL_LOST;
    }

    switch (pipeline->state)
    {
    case VIDEO_PIPELINE_STREAMING:
    case VIDEO_PIPELINE_UNSUPPORTED_INPUT:
        result = VIDEO_PIPELINE_POLL_UNCHANGED;
        break;

    case VIDEO_PIPELINE_ACQUIRING_TIMING:
        status = video_input_read_timing(&pipeline->input, &timing);
        if (status == XST_NO_DATA)
        {
            // SRC-H03: bound the wait for detected timing. If the detector
            // stalls past the timeout, restart it so an absent/flaky source
            // cannot strand ACQUIRING_TIMING forever.
            if (video_input_detector_elapsed_ms(&pipeline->input, now_ms)
                >= VIDEO_PIPELINE_TIMING_TIMEOUT_MS)
            {
                video_input_reset_detector(&pipeline->input);
                pipeline->state = VIDEO_PIPELINE_WAITING_FOR_SIGNAL;
                result = VIDEO_PIPELINE_POLL_TIMING_TIMEOUT;
                break;
            }
            result = VIDEO_PIPELINE_POLL_UNCHANGED;
            break;
        }
        if (status != XST_SUCCESS)
        {
            pipeline->state = VIDEO_PIPELINE_ERROR;
            result = VIDEO_PIPELINE_POLL_ERROR;
            break;
        }

        pipeline->input_timing = timing;
        mode = video_modes_find(timing.width, timing.height);
        if (mode == NULL)
        {
            pipeline->state = VIDEO_PIPELINE_UNSUPPORTED_INPUT;
            result = VIDEO_PIPELINE_POLL_UNSUPPORTED_INPUT;
            break;
        }

        result = start_passthrough(pipeline, mode);
        break;

    case VIDEO_PIPELINE_WAITING_FOR_SIGNAL:
    case VIDEO_PIPELINE_ERROR:
    default:
        status = video_input_start_detector(&pipeline->input, now_ms);
        if (status != XST_SUCCESS)
        {
            pipeline->state = VIDEO_PIPELINE_ERROR;
            result = VIDEO_PIPELINE_POLL_ERROR;
            break;
        }

        pipeline->state = VIDEO_PIPELINE_ACQUIRING_TIMING;
        result = VIDEO_PIPELINE_POLL_SIGNAL_DETECTED;
        break;
    }

    return result;
}

/**
 * @brief Return the current video pipeline state.
 * @param pipeline Pipeline instance to query.
 * @return Current state, or VIDEO_PIPELINE_UNINITIALIZED for NULL input.
 */
video_pipeline_state_e video_pipeline_get_state(video_pipeline_t const* const pipeline)
{
    return (pipeline != NULL) ? pipeline->state : VIDEO_PIPELINE_UNINITIALIZED;
}

/**
 * @brief Return the mode currently used for passthrough.
 * @param pipeline Pipeline instance to query.
 * @return Active mode pointer, or NULL when not streaming.
 */
video_pipeline_mode_t const* video_pipeline_get_active_mode(video_pipeline_t const* const pipeline)
{
    return (pipeline != NULL) ? pipeline->active_mode : NULL;
}

// === End of documentation ======================================================================================== //
