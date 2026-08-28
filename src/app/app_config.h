/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file app_config.h
/// @brief Centralized configuration layer - reads all env vars once at startup
///
/// All 22 SUBTITLE_* and USB_AUDIO_* environment variables are read, validated,
/// and logged here before any active object starts. HAL and service modules
/// receive configuration through typed structs, never by calling getenv().
///
/// Variables:
///  - 16 STT WebSocket (SUBTITLE_STT_*)
///  - 3 USB audio stream (USB_AUDIO_*, SUBTITLE_USB_AUDIO_AGC_*)
///  - 3 USB audio capture mixer (SUBTITLE_USB_AUDIO_CAPTURE_*, _MIXER)
///  - 2 Subtitle timeouts (SUBTITLE_*_TIMEOUT_MS)
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "stt_ws_config.h"
#include "usb_audio_capture.h"
#include "usb_audio_stream.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define APP_CONFIG_MIXER_CONTROL_MAX (64U)

// === Public data type declarations =============================================================================== //

/// USB audio capture config (now includes mixer fields that were in app_audio_capture_config_t).
typedef usb_audio_capture_config_t app_audio_capture_config_t;

/// Complete application configuration, resolved from defaults and environment.
typedef struct
{
    stt_ws_config_t stt;
    usb_audio_stream_config_t audio_stream;
    app_audio_capture_config_t audio_capture;
    uint32_t subtitle_clear_timeout_ms;
    uint32_t subtitle_partial_timeout_ms;
    uint8_t audio_agc_enabled;
    uint32_t audio_agc_target_pct;
} app_config_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Read and validate all 22 configuration variables at startup.
 *
 * Reads defaults, applies environment overrides with validation, and logs the
 * complete effective configuration. Call once before starting active objects.
 *
 * @param config Destination configuration.
 * @return 0 on success, or -EINVAL when required variables are missing/invalid.
 */
int app_config_init(app_config_t* config);

/**
 * @brief Log the complete effective configuration as one startup block.
 * @param config Resolved configuration.
 */
void app_config_log(app_config_t const* config);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
