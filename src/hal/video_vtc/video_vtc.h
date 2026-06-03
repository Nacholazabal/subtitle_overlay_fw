#pragma once

#include <stdint.h>

#include "xstatus.h"
#include "xvtc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t hps;
    uint32_t hpe;
    uint32_t hmax;
    uint32_t hpol;
    uint32_t vps;
    uint32_t vpe;
    uint32_t vmax;
    uint32_t vpol;
    double pixel_clock_mhz;
} video_vtc_mode_t;

typedef struct
{
    uint32_t width;
    uint32_t height;
} video_vtc_timing_t;

typedef struct
{
    XVtc instance;
    uint16_t device_id;
    int initialized;
} video_vtc_t;

int video_vtc_init(video_vtc_t* vtc, uint16_t device_id);
int video_vtc_configure_generator(video_vtc_t* vtc, video_vtc_mode_t const* mode);
void video_vtc_start_generator(video_vtc_t* vtc);
void video_vtc_stop_generator(video_vtc_t* vtc);
int video_vtc_start_detector(video_vtc_t* vtc);
int video_vtc_detector_locked(video_vtc_t* vtc);
int video_vtc_read_detector_timing(video_vtc_t* vtc, video_vtc_timing_t* timing);

#ifdef __cplusplus
}
#endif
