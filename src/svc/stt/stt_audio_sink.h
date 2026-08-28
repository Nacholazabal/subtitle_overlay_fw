/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_audio_sink.h
/// @brief Audio sink adapter for STT WebSocket client
///

// === Headers files inclusions ==================================================================================== //

#include "audio_sink.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //
// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Create an audio sink that submits to the active STT WebSocket client.
 * @return Audio sink wrapping the active STT client singleton.
 */
audio_sink_t stt_audio_sink_create(void);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
