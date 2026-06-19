/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file usb_audio_stream.h
/// @brief USB audio capture and TCP streaming service interface
///

// === Headers files inclusions ==================================================================================== //

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "errorno.h"
#include "usb_audio_agc.h"
#include "usb_audio_capture.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define USB_AUDIO_STREAM_DEFAULT_DEVICE "hw:0,0"
#define USB_AUDIO_STREAM_DEFAULT_HOST   "192.168.1.20"
#define USB_AUDIO_STREAM_DEFAULT_PORT   (5000U)

#define USB_AUDIO_STREAM_SAMPLE_RATE_HZ (48000U)
#define USB_AUDIO_STREAM_CHANNELS       (1U)
#define USB_AUDIO_STREAM_SAMPLE_BYTES   (2U)
#define USB_AUDIO_STREAM_CHUNK_MS       (20U)
#define USB_AUDIO_STREAM_SAMPLES_PER_CHUNK \
    ((USB_AUDIO_STREAM_SAMPLE_RATE_HZ * USB_AUDIO_STREAM_CHUNK_MS) / 1000U)
#define USB_AUDIO_STREAM_BYTES_PER_CHUNK \
    (USB_AUDIO_STREAM_SAMPLES_PER_CHUNK * USB_AUDIO_STREAM_CHANNELS * USB_AUDIO_STREAM_SAMPLE_BYTES)
#define USB_AUDIO_STREAM_QUEUE_CHUNKS (16U)
#define USB_AUDIO_STREAM_HOST_MAX_LEN (64U)

// === Public data type declarations =============================================================================== //

typedef struct
{
    char pcm_device[USB_AUDIO_CAPTURE_DEVICE_MAX_LEN];
    char tcp_host[USB_AUDIO_STREAM_HOST_MAX_LEN];
    uint32_t tcp_port;
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
    usb_audio_stream_chunk_t chunks[USB_AUDIO_STREAM_QUEUE_CHUNKS];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t dropped;
    uint8_t initialized;
} usb_audio_stream_queue_t;

typedef struct
{
    usb_audio_stream_config_t config;
    usb_audio_capture_t capture;
    usb_audio_agc_t agc;
    usb_audio_stream_queue_t queue;
    pthread_mutex_t state_mutex;
    pthread_t capture_thread;
    pthread_t sender_thread;
    uint32_t next_sequence;
    uint32_t total_dropped;
    int32_t fatal_error;
    int sender_fd;
    uint8_t stop_requested;
    uint8_t running;
    uint8_t state_initialized;
} usb_audio_stream_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Populate defaults and valid environment overrides; invalid overrides retain defaults.
void usb_audio_stream_default_config(usb_audio_stream_config_t* config);

/// @brief Open ALSA capture and start owned worker threads; may block during device setup.
/// @return 0 on success or a negative errno-style status. The instance must remain alive until stopped.
int usb_audio_stream_start(usb_audio_stream_t* stream, usb_audio_stream_config_t const* config);

/// @brief Return 0 while workers are healthy, their fatal error, or -APP_ESTATE when not running.
int usb_audio_stream_get_status(usb_audio_stream_t* stream);

/// @brief Request worker shutdown, join both threads, and release owned resources; may block while joining.
void usb_audio_stream_stop(usb_audio_stream_t* stream);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
