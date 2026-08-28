/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_audio_sink.c
/// @brief Audio sink adapter for STT WebSocket client
///

// === Headers files inclusions ==================================================================================== //

#include "stt_audio_sink.h"

#include "stt_ws_client.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

/** @brief Sink submit implementation that forwards to the active STT WS client. */
static int stt_sink_submit(void* const ctx,
                           const void* const pcm,
                           size_t const size,
                           uint64_t const timestamp_ns,
                           uint32_t const dropped)
{
    stt_ws_client_t* const client = (stt_ws_client_t*)ctx;

    if (client == NULL)
    {
        return -1; // No client available; audio is dropped.
    }

    return stt_ws_client_submit_audio(client, pcm, size, timestamp_ns, dropped);
}

audio_sink_t stt_audio_sink_create(void)
{
    audio_sink_t sink;
    sink.ctx = stt_ws_client_get_active();
    sink.submit = stt_sink_submit;
    return sink;
}

// === End of documentation ======================================================================================== //
