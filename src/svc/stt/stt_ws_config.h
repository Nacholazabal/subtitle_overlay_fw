/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_ws_config.h
/// @brief WebSocket client configuration with defaults and URL parsing
///
/// ARCH-04 will lift the getenv calls into app_config; this module will then
/// become pure defaults + URL parsing + validation.
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "stt_session_json.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define STT_WS_CONFIG_URL_MAX  (256U)
#define STT_WS_CONFIG_HOST_MAX (128U)
#define STT_WS_CONFIG_PATH_MAX (128U)
#define STT_WS_CONFIG_CA_PATH_MAX (128U)

// === Public data type declarations =============================================================================== //

/// Runtime configuration, populated from defaults and environment overrides.
typedef struct
{
    char url[STT_WS_CONFIG_URL_MAX];
    char host[STT_WS_CONFIG_HOST_MAX];
    char path[STT_WS_CONFIG_PATH_MAX];
    char ca_file[STT_WS_CONFIG_CA_PATH_MAX];
    char ca_dir[STT_WS_CONFIG_CA_PATH_MAX];
    uint16_t port;
    uint8_t use_tls;
    uint32_t connect_timeout_ms;
    uint32_t handshake_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t ping_interval_ms;
    uint32_t backoff_min_ms;
    uint32_t backoff_max_ms;
    int64_t min_epoch;
    stt_session_start_t session;
} stt_ws_config_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Fill a configuration from defaults and environment overrides.
 *
 * SUBTITLE_STT_WS_URL has no default: without it the client stays idle.
 * ARCH-04 will lift the getenv calls into app_config.
 *
 * @param config Destination configuration.
 * @return 0 on success, or -EINVAL when the URL is missing or unparsable.
 */
int stt_ws_config_default(stt_ws_config_t* config);

/**
 * @brief Parse a ws:// or wss:// URL into host, port and path.
 * @param url URL text.
 * @param config Destination; host, port, path and use_tls are written.
 * @return 0 on success, or -EINVAL when the URL is malformed.
 */
int stt_ws_config_parse_url(char const* url, stt_ws_config_t* config);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
