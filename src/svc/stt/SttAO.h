/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file SttAO.h
/// @brief Speech-to-text input active-object interface
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

/// @brief Opaque handle used to post events to the STT input active object.
extern QActive* const AO_Stt;

// === Public function declarations ================================================================================ //

/// @brief Construct the STT active object before starting it.
void stt_ao_ctor(void);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
