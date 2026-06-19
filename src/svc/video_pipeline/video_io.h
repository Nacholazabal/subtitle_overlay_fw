/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file video_io.h
/// @brief Video input and output helper interfaces
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "video_dma.h"
#include "video_dynclk.h"
#include "video_gpio.h"
#include "video_modes.h"
#include "video_vtc.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

typedef struct
{
    video_gpio_t gpio;
    video_vtc_t vtc;
    video_dma_t* dma;
    video_vtc_timing_t timing;
    uint32_t stride;
    uint32_t frame_index;
    uint32_t detector_started_ms;
    uint8_t detector_started;
    uint8_t running;
} video_input_t;

typedef struct
{
    video_dynclk_t dynclk;
    video_vtc_t vtc;
    video_dma_t* dma;
    video_pipeline_mode_t const* mode;
    uint32_t stride;
    uint32_t frame_index;
    uint8_t running;
} video_output_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int video_input_init(video_input_t* input, video_dma_t* dma, uint32_t stride);
uint8_t video_input_locked(video_input_t const* input);
int video_input_start_detector(video_input_t* input, uint32_t now_ms);
int video_input_read_timing(video_input_t* input, video_vtc_timing_t* timing);
int video_input_start_capture(video_input_t* input,
                              video_pipeline_mode_t const* mode,
                              uint32_t frame_index);
int video_input_stop(video_input_t* input);

int video_output_init(video_output_t* output, video_dma_t* dma, uint32_t stride);
int video_output_start(video_output_t* output,
                       video_pipeline_mode_t const* mode,
                       uint32_t frame_index);
int video_output_stop(video_output_t* output);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
