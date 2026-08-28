/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file app_config.c
/// @brief Centralized configuration layer implementation
///

// === Headers files inclusions ==================================================================================== //

#include "app_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "number_parse.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void read_string(char* dst, size_t dst_size, char const* env_name, char const* default_val);
static uint32_t read_u32(char const* env_name, uint32_t default_val, uint32_t min_val, uint32_t max_val);
static uint8_t read_bool(char const* env_name, uint8_t default_val);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

static void read_string(char* const dst,
                       size_t const dst_size,
                       char const* const env_name,
                       char const* const default_val)
{
    char const* const val = getenv(env_name);

    if ((val != NULL) && (val[0] != '\0'))
    {
        snprintf(dst, dst_size, "%s", val);
    }
    else if (default_val != NULL)
    {
        snprintf(dst, dst_size, "%s", default_val);
    }
    else
    {
        dst[0] = '\0';
    }
}

static uint32_t read_u32(char const* const env_name,
                        uint32_t const default_val,
                        uint32_t const min_val,
                        uint32_t const max_val)
{
    char const* const val = getenv(env_name);
    uint32_t result = default_val;

    if ((val != NULL) && (val[0] != '\0'))
    {
        if (number_parse_u32(val, strlen(val), min_val, max_val, &result) != 0)
        {
            LOG_WARNING("app_config: invalid %s='%s', using default %u", env_name, val, default_val);
            result = default_val;
        }
    }
    return result;
}

static uint8_t read_bool(char const* const env_name, uint8_t const default_val)
{
    return (uint8_t)read_u32(env_name, default_val, 0U, 1U);
}

// === Public function implementation ============================================================================== //

int app_config_init(app_config_t* const config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    memset(config, 0, sizeof(*config));

    // First populate with the existing stt_ws_config_default, which reads env vars
    // (we'll migrate those reads here incrementally to avoid breaking the build)
    if (stt_ws_config_default(&config->stt) != 0)
    {
        LOG_ERROR("app_config: STT config init failed (missing/invalid URL)");
        return -EINVAL;
    }

    // USB Audio Stream configuration (3 vars)
    usb_audio_stream_default_config(&config->audio_stream);
    config->audio_agc_enabled = read_bool("SUBTITLE_USB_AUDIO_AGC_ENABLE", 0U);
    config->audio_agc_target_pct = read_u32("SUBTITLE_USB_AUDIO_AGC_TARGET_PCT", 45U, 10U, 95U);

    // USB Audio Capture config (including mixer fields previously in HAL)
    read_string(config->audio_capture.device,
               sizeof(config->audio_capture.device),
               "USB_AUDIO_PCM_DEVICE",
               "hw:0,0");
    config->audio_capture.sample_rate_hz = 48000U;
    config->audio_capture.channels = 1U;
    config->audio_capture.samples_per_chunk = (48000U * 20U) / 1000U; // 20ms chunks

    read_string(config->audio_capture.mixer_control,
               sizeof(config->audio_capture.mixer_control),
               "SUBTITLE_USB_AUDIO_CAPTURE_CONTROL",
               "Mic");
    read_string(config->audio_capture.mixer_device,
               sizeof(config->audio_capture.mixer_device),
               "SUBTITLE_USB_AUDIO_MIXER",
               "");
    config->audio_capture.volume_pct = read_u32("SUBTITLE_USB_AUDIO_CAPTURE_VOL_PCT", 100U, 0U, 100U);

    // Subtitle timeouts (2 vars)
    config->subtitle_clear_timeout_ms = read_u32("SUBTITLE_CLEAR_TIMEOUT_MS", 5000U, 0U, 60000U);
    config->subtitle_partial_timeout_ms = read_u32("SUBTITLE_PARTIAL_TIMEOUT_MS", 1500U, 0U, 10000U);

    app_config_log(config);
    return 0;
}

void app_config_log(app_config_t const* const config)
{
    if (config == NULL)
    {
        return;
    }

    LOG_INFO("========== Application Configuration ==========");
    LOG_INFO("STT WebSocket: %s://%s:%u%s",
             (config->stt.use_tls != 0U) ? "wss" : "ws",
             config->stt.host,
             (unsigned)config->stt.port,
             config->stt.path);
    if (config->stt.use_tls != 0U)
    {
        LOG_INFO("  TLS CA: file='%s' dir='%s'", config->stt.ca_file, config->stt.ca_dir);
    }
    LOG_INFO("  Timeouts: connect=%u handshake=%u idle=%u ping=%u ms",
             config->stt.connect_timeout_ms,
             config->stt.handshake_timeout_ms,
             config->stt.idle_timeout_ms,
             config->stt.ping_interval_ms);
    LOG_INFO("  Backoff: %u-%u ms", config->stt.backoff_min_ms, config->stt.backoff_max_ms);
    LOG_INFO("  Session: lang=%s chunk_ms=%u latency_ms=%u",
             config->stt.session.target_lang,
             config->stt.session.chunk_ms,
             config->stt.session.latency_ms);

    LOG_INFO("USB Audio Stream: device=%s", config->audio_stream.pcm_device);
    LOG_INFO("  AGC: %s (target=%u%%)",
             (config->audio_agc_enabled != 0U) ? "enabled" : "disabled",
             config->audio_agc_target_pct);

    LOG_INFO("USB Audio Capture: device=%s", config->audio_capture.device);
    LOG_INFO("  Mixer: control='%s' device='%s' volume=%u%%",
             config->audio_capture.mixer_control,
             config->audio_capture.mixer_device,
             config->audio_capture.volume_pct);

    LOG_INFO("Subtitle Timeouts: clear=%u partial=%u ms",
             config->subtitle_clear_timeout_ms,
             config->subtitle_partial_timeout_ms);
    LOG_INFO("===============================================");
}

// === End of documentation ======================================================================================== //
