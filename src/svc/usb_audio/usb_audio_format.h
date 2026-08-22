/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file usb_audio_format.h
/// @brief Shared PCM format constants for USB capture, optical playback, and STT.
///

#define USB_AUDIO_SAMPLE_RATE_HZ          (48000U)
#define USB_AUDIO_CAPTURE_CHANNELS        (2U)
#define USB_AUDIO_PLAYBACK_CHANNELS       (2U)
#define USB_AUDIO_STT_CHANNELS            (1U)
#define USB_AUDIO_SAMPLE_BYTES            (2U)
#define USB_AUDIO_CHUNK_MS                (20U)
#define USB_AUDIO_FRAMES_PER_CHUNK        ((USB_AUDIO_SAMPLE_RATE_HZ * USB_AUDIO_CHUNK_MS) / 1000U)
#define USB_AUDIO_CAPTURE_SAMPLES         (USB_AUDIO_FRAMES_PER_CHUNK * USB_AUDIO_CAPTURE_CHANNELS)
#define USB_AUDIO_CAPTURE_BYTES_PER_CHUNK (USB_AUDIO_CAPTURE_SAMPLES * USB_AUDIO_SAMPLE_BYTES)
#define USB_AUDIO_STT_BYTES_PER_CHUNK \
    (USB_AUDIO_FRAMES_PER_CHUNK * USB_AUDIO_STT_CHANNELS * USB_AUDIO_SAMPLE_BYTES)
