/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file video_pipeline.h
/// @brief Video pipeline service interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "video_dma.h"
#include "video_io.h"
#include "video_modes.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define VIDEO_PIPELINE_FRAME_COUNT     3U
#define VIDEO_PIPELINE_MAX_WIDTH       1920U
#define VIDEO_PIPELINE_MAX_HEIGHT      1080U
#define VIDEO_PIPELINE_BYTES_PER_PIXEL 3U
#define VIDEO_PIPELINE_STRIDE          (VIDEO_PIPELINE_MAX_WIDTH * VIDEO_PIPELINE_BYTES_PER_PIXEL)

// === Public data type declarations =============================================================================== //

typedef enum
{
    VIDEO_PIPELINE_UNINITIALIZED = 0,
    VIDEO_PIPELINE_WAITING_FOR_SIGNAL,
    VIDEO_PIPELINE_ACQUIRING_TIMING,
    VIDEO_PIPELINE_STREAMING,
    VIDEO_PIPELINE_UNSUPPORTED_INPUT,
    VIDEO_PIPELINE_ERROR,
} video_pipeline_state_e;

typedef enum
{
    VIDEO_PIPELINE_POLL_UNCHANGED = 0,
    VIDEO_PIPELINE_POLL_SIGNAL_DETECTED,
    VIDEO_PIPELINE_POLL_STREAMING_STARTED,
    VIDEO_PIPELINE_POLL_SIGNAL_LOST,
    VIDEO_PIPELINE_POLL_UNSUPPORTED_INPUT,
    VIDEO_PIPELINE_POLL_ERROR,
} video_pipeline_poll_result_e;

typedef struct
{
    video_dma_t dma;
    uint8_t* frames[VIDEO_PIPELINE_FRAME_COUNT];
    video_output_t output;
    video_input_t input;
    video_pipeline_mode_t const* active_mode;
    video_vtc_timing_t input_timing;
    video_pipeline_state_e state;
    uint32_t active_frame;
    uint8_t platform_ready;
} video_pipeline_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int video_pipeline_init(video_pipeline_t* pipeline);
void video_pipeline_cleanup(video_pipeline_t* pipeline);
video_pipeline_poll_result_e video_pipeline_poll(video_pipeline_t* pipeline, uint32_t now_ms);
video_pipeline_state_e video_pipeline_get_state(video_pipeline_t const* pipeline);
video_pipeline_mode_t const* video_pipeline_get_active_mode(video_pipeline_t const* pipeline);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
