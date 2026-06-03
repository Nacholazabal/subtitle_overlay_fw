#pragma once

#include "video_dma.h"
#include "video_dynclk.h"
#include "video_modes.h"
#include "video_vtc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    video_dynclk_t dynclk;
    video_vtc_t vtc;
    video_dma_t* dma;
    video_pipeline_mode_t const* mode;
    uint32_t stride;
    uint32_t frame_index;
    int running;
} video_output_t;

int video_output_init(video_output_t* output, video_dma_t* dma, uint32_t stride);
int video_output_start(video_output_t* output, video_pipeline_mode_t const* mode, uint32_t frame_index);
int video_output_stop(video_output_t* output);

#ifdef __cplusplus
}
#endif
