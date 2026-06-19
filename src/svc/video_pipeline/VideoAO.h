/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file VideoAO.h
/// @brief Video pipeline active-object interface
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //
// === Public variable declarations ================================================================================ //

/// @brief Opaque handle used to post events to the video pipeline active object.
extern QActive* const AO_Video;

// === Public function declarations ================================================================================ //

/// @brief Construct the video_ao_t state machine before starting it.
void video_ao_ctor(void);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
