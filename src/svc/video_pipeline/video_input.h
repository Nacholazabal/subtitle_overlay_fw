#pragma once

#include "video_dma.h"
#include "video_gpio.h"
#include "video_modes.h"
#include "video_vtc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    video_gpio_t gpio;
    video_vtc_t vtc;
    video_dma_t* dma;
    video_vtc_timing_t timing;
    uint32_t stride;
    uint32_t frame_index;
    uint32_t detector_started_ms;
    int detector_started;
    int running;
} video_input_t;

int video_input_init(video_input_t* input, video_dma_t* dma, uint32_t stride);
int video_input_locked(video_input_t const* input);
int video_input_start_detector(video_input_t* input, uint32_t now_ms);
int video_input_read_timing(video_input_t* input, video_vtc_timing_t* timing);
int video_input_start_capture(video_input_t* input, video_pipeline_mode_t const* mode, uint32_t frame_index);
int video_input_stop(video_input_t* input);

#ifdef __cplusplus
}
#endif
