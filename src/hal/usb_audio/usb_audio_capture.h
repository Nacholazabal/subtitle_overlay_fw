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

/// Recovery attempts allowed inside a single 20 ms chunk read.
#define USB_AUDIO_CAPTURE_MAX_RECOVERIES (4U)

// === Public data type declarations =============================================================================== //

/// @brief What a read should do after ALSA reported a non-positive frame count.
typedef enum
{
    USB_AUDIO_CAPTURE_RECOVERY_RETRY = 0, ///< Transient: recover the PCM and read again.
    USB_AUDIO_CAPTURE_RECOVERY_ABORT,     ///< A stop was requested; leave promptly.
    USB_AUDIO_CAPTURE_RECOVERY_FAIL,      ///< Unrecoverable, or the retry budget is spent.
} usb_audio_capture_recovery_e;

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
    volatile uint8_t abort_requested; ///< Set by ::usb_audio_capture_abort, read by the read loop.
} usb_audio_capture_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int usb_audio_capture_init(usb_audio_capture_t* capture, usb_audio_capture_config_t const* config);

/**
 * @brief Decide how a read should proceed after ALSA returned @p err.
 * @param err Negative errno-style value reported by the ALSA read.
 * @param attempts Recoveries already made during this chunk read.
 * @param abort_requested Nonzero once a stop has been requested.
 * @return The action the read loop must take.
 */
usb_audio_capture_recovery_e usb_audio_capture_recovery_decision(int err,
                                                                uint32_t attempts,
                                                                uint8_t abort_requested);

/// @return 0 on success, -ECANCELED when aborted, or another negative errno-style value.
int usb_audio_capture_read_chunk(usb_audio_capture_t* capture,
                                 uint8_t* dst,
                                 size_t dst_size,
                                 size_t* bytes_read);

/// @brief Make an in-flight read give up and unblock it. Safe from another thread.
void usb_audio_capture_abort(usb_audio_capture_t* capture);
void usb_audio_capture_cleanup(usb_audio_capture_t* capture);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
