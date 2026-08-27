/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_ws_config.c
/// @brief WebSocket client configuration with defaults and URL parsing
///

// === Headers files inclusions ==================================================================================== //

#include "stt_ws_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "number_parse.h"

// === Macros definitions ========================================================================================== //

#define STT_WS_PORT_MAX (65535U)

#define STT_WS_DEFAULT_CONNECT_TIMEOUT_MS   (5000U)
#define STT_WS_DEFAULT_HANDSHAKE_TIMEOUT_MS (10000U)
#define STT_WS_DEFAULT_IDLE_TIMEOUT_MS      (45000U)
#define STT_WS_DEFAULT_PING_INTERVAL_MS     (15000U)
#define STT_WS_DEFAULT_BACKOFF_MIN_MS       (500U)
#define STT_WS_DEFAULT_BACKOFF_MAX_MS       (30000U)
#define STT_WS_DEFAULT_CA_FILE              "/etc/ssl/certs/ca-certificates.crt"
#define STT_WS_DEFAULT_MIN_EPOCH            (1767225600L)

#define STT_WS_DEFAULT_SAMPLE_RATE_HZ  (48000U)
#define STT_WS_DEFAULT_CHUNK_MS        (20U)
#define STT_WS_DEFAULT_LATENCY_MS      (560U)
#define STT_WS_DEFAULT_STOP_HISTORY_MS (600U)
#define STT_WS_DEFAULT_RESIDUE_TOKENS  (2U)
#define STT_WS_DEFAULT_TARGET_LANG     "es-ES"

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void env_string(char* dst, size_t dst_size, char const* name);
static uint32_t env_u32(char const* name, uint32_t fallback, uint32_t min_value, uint32_t max_value);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

static void env_string(char* const dst, size_t dst_size, char const* const name)
{
    char const* const value = getenv(name);

    if ((value != NULL) && (value[0] != '\0'))
    {
        snprintf(dst, dst_size, "%s", value);
    }
}

static uint32_t env_u32(char const* const name,
                        uint32_t fallback,
                        uint32_t min_value,
                        uint32_t max_value)
{
    char const* const value = getenv(name);
    uint32_t parsed;

    if ((value == NULL) || (value[0] == '\0'))
    {
        return fallback;
    }
    if (number_parse_u32(value, strlen(value), min_value, max_value, &parsed) != 0)
    {
        LOG_WARNING("stt-ws-config: ignoring invalid %s='%s'", name, value);
        return fallback;
    }

    return parsed;
}

// === Public function implementation ============================================================================== //

int stt_ws_config_parse_url(char const* const url, stt_ws_config_t* const config)
{
    char const* cursor;
    char const* host_end;
    char const* path_start;
    size_t host_len;
    uint8_t use_tls;
    uint32_t port;

    if ((url == NULL) || (config == NULL))
    {
        return -EINVAL;
    }

    if (strncmp(url, "wss://", 6U) == 0)
    {
        use_tls = 1U;
        port = 443U;
        cursor = &url[6];
    }
    else if (strncmp(url, "ws://", 5U) == 0)
    {
        use_tls = 0U;
        port = 80U;
        cursor = &url[5];
    }
    else
    {
        return -EINVAL;
    }

    path_start = strchr(cursor, '/');
    host_end = (path_start != NULL) ? path_start : (cursor + strlen(cursor));

    {
        char const* const colon = memchr(cursor, ':', (size_t)(host_end - cursor));

        if (colon != NULL)
        {
            if (number_parse_u32(colon + 1, (size_t)(host_end - (colon + 1)), 1U, STT_WS_PORT_MAX,
                                 &port)
                != 0)
            {
                return -EINVAL;
            }
            host_len = (size_t)(colon - cursor);
        }
        else
        {
            host_len = (size_t)(host_end - cursor);
        }
    }

    if ((host_len == 0U) || (host_len >= sizeof(config->host)))
    {
        return -EINVAL;
    }

    memcpy(config->host, cursor, host_len);
    config->host[host_len] = '\0';
    config->port = (uint16_t)port;
    config->use_tls = use_tls;
    snprintf(config->path, sizeof(config->path), "%s",
             (path_start != NULL) ? path_start : "/");

    return 0;
}

int stt_ws_config_default(stt_ws_config_t* const config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    memset(config, 0, sizeof(*config));
    snprintf(config->ca_file, sizeof(config->ca_file), "%s", STT_WS_DEFAULT_CA_FILE);
    config->connect_timeout_ms = STT_WS_DEFAULT_CONNECT_TIMEOUT_MS;
    config->handshake_timeout_ms = STT_WS_DEFAULT_HANDSHAKE_TIMEOUT_MS;
    config->idle_timeout_ms = STT_WS_DEFAULT_IDLE_TIMEOUT_MS;
    config->ping_interval_ms = STT_WS_DEFAULT_PING_INTERVAL_MS;
    config->backoff_min_ms = STT_WS_DEFAULT_BACKOFF_MIN_MS;
    config->backoff_max_ms = STT_WS_DEFAULT_BACKOFF_MAX_MS;
    config->min_epoch = STT_WS_DEFAULT_MIN_EPOCH;

    config->session.sample_rate_hz = STT_WS_DEFAULT_SAMPLE_RATE_HZ;
    config->session.channels = 1U;
    config->session.format = STT_SESSION_FORMAT_S16_LE;
    config->session.chunk_ms = STT_WS_DEFAULT_CHUNK_MS;
    config->session.samples_per_chunk =
        (STT_WS_DEFAULT_SAMPLE_RATE_HZ * STT_WS_DEFAULT_CHUNK_MS) / 1000U;
    config->session.bytes_per_chunk = config->session.samples_per_chunk * 2U;
    config->session.latency_ms = STT_WS_DEFAULT_LATENCY_MS;
    config->session.stop_history_eou_ms = STT_WS_DEFAULT_STOP_HISTORY_MS;
    config->session.residue_tokens_at_end = STT_WS_DEFAULT_RESIDUE_TOKENS;
    snprintf(config->session.target_lang, sizeof(config->session.target_lang), "%s",
             STT_WS_DEFAULT_TARGET_LANG);

    env_string(config->url, sizeof(config->url), "SUBTITLE_STT_WS_URL");
    env_string(config->ca_file, sizeof(config->ca_file), "SUBTITLE_STT_WS_CA_FILE");
    env_string(config->ca_dir, sizeof(config->ca_dir), "SUBTITLE_STT_WS_CA_DIR");
    env_string(config->session.target_lang, sizeof(config->session.target_lang),
               "SUBTITLE_STT_NEMOTRON_TARGET_LANG");

    config->connect_timeout_ms =
        env_u32("SUBTITLE_STT_WS_CONNECT_TIMEOUT_MS", config->connect_timeout_ms, 100U, 60000U);
    config->handshake_timeout_ms =
        env_u32("SUBTITLE_STT_WS_HANDSHAKE_TIMEOUT_MS", config->handshake_timeout_ms, 100U, 60000U);
    config->idle_timeout_ms =
        env_u32("SUBTITLE_STT_WS_IDLE_TIMEOUT_MS", config->idle_timeout_ms, 1000U, 600000U);
    config->ping_interval_ms =
        env_u32("SUBTITLE_STT_WS_PING_MS", config->ping_interval_ms, 0U, 600000U);
    config->backoff_min_ms =
        env_u32("SUBTITLE_STT_WS_BACKOFF_MIN_MS", config->backoff_min_ms, 100U, 60000U);
    config->backoff_max_ms =
        env_u32("SUBTITLE_STT_WS_BACKOFF_MAX_MS", config->backoff_max_ms, 1000U, 600000U);
    config->min_epoch = (int64_t)env_u32("SUBTITLE_STT_MIN_EPOCH",
                                         (uint32_t)STT_WS_DEFAULT_MIN_EPOCH, 0U, UINT32_MAX);
    config->session.latency_ms =
        env_u32("SUBTITLE_STT_NEMOTRON_LATENCY_MS", config->session.latency_ms, 0U, 10000U);
    config->session.stop_history_eou_ms = env_u32("SUBTITLE_STT_NEMOTRON_STOP_HISTORY_EOU_MS",
                                                  config->session.stop_history_eou_ms, 0U, 10000U);
    config->session.residue_tokens_at_end =
        env_u32("SUBTITLE_STT_NEMOTRON_RESIDUE_TOKENS_AT_END",
                config->session.residue_tokens_at_end, 0U, 100U);

    if (config->backoff_max_ms < config->backoff_min_ms)
    {
        config->backoff_max_ms = config->backoff_min_ms;
    }

    if (config->url[0] == '\0')
    {
        LOG_ERROR("stt-ws-config: SUBTITLE_STT_WS_URL is not set; the STT link stays down");
        return -EINVAL;
    }

    return stt_ws_config_parse_url(config->url, config);
}
