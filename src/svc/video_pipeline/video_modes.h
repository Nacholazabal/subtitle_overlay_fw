/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file video_modes.h
/// @brief Supported video mode table interface
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

#include "video_vtc.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

typedef struct
{
    char label[32];
    video_vtc_mode_t timing;
} video_pipeline_mode_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

video_pipeline_mode_t const* video_modes_default(void);
video_pipeline_mode_t const* video_modes_find(uint32_t width, uint32_t height);
video_pipeline_mode_t const* video_modes_all(size_t* count);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
