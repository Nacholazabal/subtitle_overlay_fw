#pragma once

#include <stddef.h>
#include <stdint.h>

#include "video_vtc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char label[32];
    video_vtc_mode_t timing;
} video_pipeline_mode_t;

video_pipeline_mode_t const* video_modes_default(void);
video_pipeline_mode_t const* video_modes_find(uint32_t width, uint32_t height);
video_pipeline_mode_t const* video_modes_all(size_t* count);

#ifdef __cplusplus
}
#endif
