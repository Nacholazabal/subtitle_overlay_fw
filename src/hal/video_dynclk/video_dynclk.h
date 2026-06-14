/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file video_dynclk.h
/// @brief Dynamic pixel clock HAL adapter interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "xstatus.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

typedef struct
{
    uintptr_t base;
    double actual_frequency_mhz;
} video_dynclk_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int video_dynclk_init(video_dynclk_t* dynclk);
/*
 * These calls perform short bounded MMIO polling while the clock locks/stops.
 * Keep them out of high-frequency AO handlers unless that bounded latency is
 * acceptable for the state transition.
 */
int video_dynclk_configure(video_dynclk_t* dynclk, double frequency_mhz);
int video_dynclk_stop(video_dynclk_t* dynclk);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
