#pragma once

#include <stdint.h>

#include "xstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uintptr_t base;
    double actual_frequency_mhz;
} video_dynclk_t;

int video_dynclk_init(video_dynclk_t* dynclk);
int video_dynclk_configure(video_dynclk_t* dynclk, double frequency_mhz);
int video_dynclk_stop(video_dynclk_t* dynclk);

#ifdef __cplusplus
}
#endif
