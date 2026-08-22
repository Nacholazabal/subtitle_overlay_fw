/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file usb_audio_stream.h
/// @brief USB audio capture and nonblocking handoff to the STT subsystem
///

// === Headers files inclusions ==================================================================================== //

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "errorno.h"
#include "usb_audio_agc.h"
#include "usb_audio_capture.h"
#include "usb_audio_passthrough.h"
#include "usb_audio_playback.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define USB_AUDIO_STREAM_DEFAULT_DEVICE              "hw:0,0"
#define USB_AUDIO_STREAM_DEFAULT_PLAYBACK_VOLUME_PCT (100U)

// === Public data type declarations =============================================================================== //

typedef struct
{
    char pcm_device[USB_AUDIO_CAPTURE_DEVICE_MAX_LEN];
    char playback_pcm_device[USB_AUDIO_CAPTURE_DEVICE_MAX_LEN];
    uint32_t playback_volume_pct;
    uint8_t passthrough_enabled;
} usb_audio_stream_config_t;

typedef struct
{
    uint8_t payload[USB_AUDIO_STT_BYTES_PER_CHUNK];
    uint64_t timestamp_ns;
    uint32_t sequence;
    uint32_t bytes_used;
} usb_audio_stream_chunk_t;

typedef struct
{
    usb_audio_stream_config_t config;
    usb_audio_capture_t capture;
    usb_audio_playback_t playback;
    usb_audio_passthrough_queue_t passthrough_queue;
    usb_audio_agc_t agc;
    uint8_t agc_enabled;
    pthread_mutex_t state_mutex;
    pthread_cond_t playback_cond;
    pthread_t capture_thread;
    pthread_t playback_thread;
    uint32_t next_sequence;
    uint32_t total_dropped;
    uint32_t playback_chunks_written;
    uint32_t playback_chunks_dropped;
    uint32_t playback_recoveries;
    int32_t fatal_error;
    int32_t playback_error;
    uint8_t stop_requested;
    uint8_t playback_active;
    uint8_t playback_thread_started;
    uint8_t playback_cond_initialized;
    uint8_t running;
    uint8_t state_initialized;
} usb_audio_stream_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Populate defaults and valid environment overrides; invalid overrides retain defaults.
void usb_audio_stream_default_config(usb_audio_stream_config_t* config);

/// @brief Open ALSA capture and start its capture worker; may block during device setup.
/// Captured chunks are submitted nonblocking to the STT subsystem's bounded queue.
/// @return 0 on success or a negative errno-style status. The instance must remain alive until stopped.
int usb_audio_stream_start(usb_audio_stream_t* stream, usb_audio_stream_config_t const* config);

/// @brief Return 0 while workers are healthy, their fatal error, or -APP_ESTATE when not running.
int usb_audio_stream_get_status(usb_audio_stream_t* stream);

/// @brief Request capture shutdown, join its thread, and release ALSA resources.
void usb_audio_stream_stop(usb_audio_stream_t* stream);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
