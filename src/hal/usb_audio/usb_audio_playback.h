/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file usb_audio_playback.h
/// @brief Linux ALSA USB audio playback adapter used by the optical S/PDIF path.
///

#include <stddef.h>
#include <stdint.h>

#include "usb_audio_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char device[USB_AUDIO_CAPTURE_DEVICE_MAX_LEN];
    uint32_t sample_rate_hz;
    uint32_t channels;
    uint32_t frames_per_chunk;
    uint32_t volume_pct;
} usb_audio_playback_config_t;

typedef struct
{
    usb_audio_playback_config_t config;
    void* pcm_handle;
    uint32_t bytes_per_frame;
    uint32_t recoveries;
    uint8_t initialized;
} usb_audio_playback_t;

int usb_audio_playback_init(usb_audio_playback_t* playback,
                            usb_audio_playback_config_t const* config);
int usb_audio_playback_write_chunk(usb_audio_playback_t* playback,
                                   uint8_t const* src,
                                   size_t src_size);
void usb_audio_playback_abort(usb_audio_playback_t* playback);
void usb_audio_playback_cleanup(usb_audio_playback_t* playback);

#ifdef __cplusplus
}
#endif
