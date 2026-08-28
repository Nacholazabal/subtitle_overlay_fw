/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file audio_sink.h
/// @brief Audio data sink interface for dependency inversion
///
/// Allows the audio capture path to remain agnostic to its consumer (STT client,
/// file recorder, etc.). The producer calls submit(); the implementation decides
/// where the audio goes.
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

/// @brief Audio data sink: injected interface for submitting captured audio.
typedef struct
{
    void* ctx; ///< Implementation-defined context pointer.
    /// @brief Submit one audio chunk.
    /// @param ctx Implementation context.
    /// @param pcm PCM audio data (mono S16LE).
    /// @param size PCM data size in bytes.
    /// @param timestamp_ns Capture timestamp (monotonic nanoseconds).
    /// @param dropped Cumulative dropped-chunk count since stream start.
    /// @return 0 on success, or a negative errno-style value on fatal error.
    int (*submit)(void* ctx, const void* pcm, size_t size, uint64_t timestamp_ns, uint32_t dropped);
} audio_sink_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //
// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
