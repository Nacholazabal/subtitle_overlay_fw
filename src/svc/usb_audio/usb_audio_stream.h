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

#include "audio_sink.h"
#include "errorno.h"
#include "usb_audio_agc.h"
#include "usb_audio_capture.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define USB_AUDIO_STREAM_DEFAULT_DEVICE "hw:0,0"

#define USB_AUDIO_STREAM_SAMPLE_RATE_HZ (48000U)
#define USB_AUDIO_STREAM_CHANNELS       (1U)
#define USB_AUDIO_STREAM_SAMPLE_BYTES   (2U)
#define USB_AUDIO_STREAM_CHUNK_MS       (20U)
#define USB_AUDIO_STREAM_SAMPLES_PER_CHUNK \
    ((USB_AUDIO_STREAM_SAMPLE_RATE_HZ * USB_AUDIO_STREAM_CHUNK_MS) / 1000U)
#define USB_AUDIO_STREAM_BYTES_PER_CHUNK \
    (USB_AUDIO_STREAM_SAMPLES_PER_CHUNK * USB_AUDIO_STREAM_CHANNELS * USB_AUDIO_STREAM_SAMPLE_BYTES)

// === Public data type declarations =============================================================================== //

typedef struct
{
    char pcm_device[USB_AUDIO_CAPTURE_DEVICE_MAX_LEN];
} usb_audio_stream_config_t;

typedef struct
{
    uint8_t payload[USB_AUDIO_STREAM_BYTES_PER_CHUNK];
    uint64_t timestamp_ns;
    uint32_t sequence;
    uint32_t bytes_used;
} usb_audio_stream_chunk_t;

typedef struct
{
    usb_audio_stream_config_t config;
    usb_audio_capture_t capture;
    usb_audio_agc_t agc;
    uint8_t agc_enabled;
    audio_sink_t sink;
    pthread_mutex_t state_mutex;
    pthread_t capture_thread;
    uint32_t next_sequence;
    uint32_t total_dropped;
    int32_t fatal_error;
    uint8_t stop_requested;
    uint8_t worker_done; ///< Set by the capture thread just before it returns.
    uint8_t running;
    uint8_t state_initialized;
} usb_audio_stream_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Populate defaults and valid environment overrides; invalid overrides retain defaults.
void usb_audio_stream_default_config(usb_audio_stream_config_t* config);

/// @brief Open ALSA capture and start its capture worker; may block during device setup.
/// Captured chunks are submitted to the provided sink.
/// @param stream Stream service instance.
/// @param config Capture configuration.
/// @param sink Audio data sink (injected dependency).
/// @return 0 on success or a negative errno-style status. The instance must remain alive until stopped.
int usb_audio_stream_start(usb_audio_stream_t* stream, usb_audio_stream_config_t const* config, audio_sink_t const* sink);

/// @brief Return 0 while workers are healthy, their fatal error, or -APP_ESTATE when not running.
int usb_audio_stream_get_status(usb_audio_stream_t* stream);

/* Shutdown is request/observe/join so a QP/C state handler never blocks on it. */

/// @brief Ask the capture worker to stop and unblock it. Always nonblocking.
void usb_audio_stream_request_stop(usb_audio_stream_t* stream);

/// @brief Return nonzero once the capture worker has left its loop and can be joined.
uint8_t usb_audio_stream_stop_complete(usb_audio_stream_t* stream);

/**
 * @brief Join the stopped worker and release ALSA resources.
 *
 * Call once ::usb_audio_stream_stop_complete reports completion; the join is then
 * immediate.
 * @param stream Stream service instance.
 * @return 0 when joined or nothing to join, or -EAGAIN while the worker is live.
 */
int usb_audio_stream_finish_stop(usb_audio_stream_t* stream);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
