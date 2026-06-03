/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file video_vtc.h
/// @brief Video Timing Controller HAL adapter interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "xstatus.h"
#include "xvtc.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

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

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int video_vtc_init(video_vtc_t* vtc, uint16_t device_id);
int video_vtc_configure_generator(video_vtc_t* vtc, video_vtc_mode_t const* mode);
void video_vtc_start_generator(video_vtc_t* vtc);
void video_vtc_stop_generator(video_vtc_t* vtc);
int video_vtc_start_detector(video_vtc_t* vtc);
int video_vtc_detector_locked(video_vtc_t* vtc);
int video_vtc_read_detector_timing(video_vtc_t* vtc, video_vtc_timing_t* timing);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
