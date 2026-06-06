/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file USBAudioAO.h
/// @brief USB audio capture active-object interface
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

/// @brief Opaque handle used to post events to the USB audio active object.
extern QActive* const AO_USBAudio;

// === Public function declarations ================================================================================ //

/// @brief Construct the USB audio active object before starting it.
void usb_audio_ao_ctor(void);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
