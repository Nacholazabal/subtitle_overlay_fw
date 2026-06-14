/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file video_dma.h
/// @brief HDMI VDMA HAL adapter interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>
#include <stddef.h>

#include "xstatus.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define VIDEO_DMA_MAX_FRAMES    3U
#define VIDEO_DMA_STATUS_HALTED 0x00000001U
#define VIDEO_DMA_STATUS_IDLE   0x00000002U

// === Public data type declarations =============================================================================== //

typedef enum
{
    VIDEO_DMA_CHANNEL_MM2S = 0,
    VIDEO_DMA_CHANNEL_S2MM,
} video_dma_channel_e;

typedef struct
{
    int fd;
    int is_open;
    uint8_t* frames[VIDEO_DMA_MAX_FRAMES];
    uint32_t frame_count;
    uint32_t frame_size;
    size_t mmap_size;
} video_dma_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int video_dma_init(video_dma_t* dma, uint8_t* frames[VIDEO_DMA_MAX_FRAMES], uint32_t frame_count);
void video_dma_cleanup(video_dma_t* dma);
int video_dma_configure(video_dma_t* dma,
                        video_dma_channel_e channel,
                        uint32_t width,
                        uint32_t height,
                        uint32_t stride,
                        uint32_t frame_index);
int video_dma_start(video_dma_t* dma, video_dma_channel_e channel);
int video_dma_stop(video_dma_t* dma, video_dma_channel_e channel);
int video_dma_select_frame(video_dma_t* dma, video_dma_channel_e channel, uint32_t frame_index);
uint32_t video_dma_status(video_dma_t* dma, video_dma_channel_e channel);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
