/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file usb_audio_capture.h
/// @brief Linux ALSA USB audio capture adapter interface
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define USB_AUDIO_CAPTURE_DEVICE_MAX_LEN (64U)

// === Public data type declarations =============================================================================== //

typedef struct
{
    char device[USB_AUDIO_CAPTURE_DEVICE_MAX_LEN];
    uint32_t sample_rate_hz;
    uint32_t channels;
    uint32_t samples_per_chunk;
} usb_audio_capture_config_t;

typedef struct
{
    usb_audio_capture_config_t config;
    void* pcm_handle;
    uint32_t bytes_per_frame;
    uint8_t initialized;
} usb_audio_capture_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int usb_audio_capture_init(usb_audio_capture_t* capture, usb_audio_capture_config_t const* config);
int usb_audio_capture_read_chunk(usb_audio_capture_t* capture,
                                 uint8_t* dst,
                                 size_t dst_size,
                                 size_t* bytes_read);
void usb_audio_capture_abort(usb_audio_capture_t* capture);
void usb_audio_capture_cleanup(usb_audio_capture_t* capture);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
