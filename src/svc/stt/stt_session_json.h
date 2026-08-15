/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_session_json.h
/// @brief Session control messages of the streaming STT WebSocket protocol
///
/// Wire contract (protocol version 1, mirrored by `server/runtime/protocol.py`):
///  * the client sends `session_start` as the first text frame and the server
///    answers `session_ready` before any audio may flow;
///  * `session_ready.run_config` is the server's effective configuration and is
///    captured verbatim so the firmware can log what it actually negotiated;
///  * an `error` message ends the session; `busy` marks the server's
///    single-GPU-session rejection, which is retryable.
///
/// Transcript payloads are **not** handled here: they keep going through
/// `stt_event_rx_parse_line()` so both paths share one parser.
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

/// Protocol version this firmware speaks; a server answering anything else is refused.
#define STT_SESSION_PROTOCOL_VERSION (1U)
/// Audio format identifier for signed 16-bit little-endian PCM.
#define STT_SESSION_FORMAT_S16_LE (1U)

#define STT_SESSION_LANG_MAX (16U)
#define STT_SESSION_ENGINE_MAX (48U)
/// The server's `run_config` measured 619 bytes; keep headroom for added keys.
#define STT_SESSION_RUN_CONFIG_MAX (768U)
#define STT_SESSION_ERROR_MAX (160U)

// === Public data type declarations =============================================================================== //

/// @brief Session control messages this client understands.
typedef enum
{
    STT_SESSION_MSG_UNKNOWN = 0,
    STT_SESSION_MSG_SESSION_READY,
    STT_SESSION_MSG_TRANSCRIPT,
    STT_SESSION_MSG_SESSION_SUMMARY,
    STT_SESSION_MSG_ERROR,
    STT_SESSION_MSG_PONG,
} stt_session_msg_e;

/// @brief Everything `session_start` must declare about the audio stream.
typedef struct
{
    uint32_t sample_rate_hz;
    uint32_t channels;
    uint32_t format;
    uint32_t chunk_ms;
    uint32_t samples_per_chunk;
    uint32_t bytes_per_chunk;
    /// Backend overrides; a zero numeric field or empty language is omitted.
    uint32_t latency_ms;
    uint32_t stop_history_eou_ms;
    uint32_t residue_tokens_at_end;
    char target_lang[STT_SESSION_LANG_MAX];
} stt_session_start_t;

/// @brief Fields taken from `session_ready`.
typedef struct
{
    uint32_t version;
    uint32_t sample_rate_hz;
    char run_engine[STT_SESSION_ENGINE_MAX];
    /// Raw JSON text of `run_config`, for the effective-configuration log line.
    char run_config[STT_SESSION_RUN_CONFIG_MAX];
    uint8_t run_config_truncated;
} stt_session_ready_t;

/// @brief Fields taken from an `error` message.
typedef struct
{
    char message[STT_SESSION_ERROR_MAX];
    uint8_t busy; ///< Server already has an active session; retry later.
} stt_session_error_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Serialize the `session_start` opening message.
 * @param out Destination buffer; NUL-terminated on success.
 * @param out_size Destination capacity.
 * @param start Stream description and backend overrides.
 * @return Message length in bytes, or a negative errno-style value.
 */
int stt_session_json_build_start(char* out, size_t out_size, stt_session_start_t const* start);

/**
 * @brief Identify a server message by its top-level `type` field.
 * @param json NUL-terminated JSON object.
 * @return The message kind, or ::STT_SESSION_MSG_UNKNOWN when absent or unknown.
 */
stt_session_msg_e stt_session_json_message_type(char const* json);

/**
 * @brief Parse and validate a `session_ready` message.
 * @param json NUL-terminated JSON object.
 * @param ready Destination, zeroed before use.
 * @return 0 on success, or -EPROTO when the message is malformed, is not
 *         `session_ready`, or declares an unsupported protocol version.
 */
int stt_session_json_parse_ready(char const* json, stt_session_ready_t* ready);

/**
 * @brief Parse an `error` message for logging and retry classification.
 * @param json NUL-terminated JSON object.
 * @param error Destination, zeroed before use.
 * @return 0 on success, or -EPROTO when the message is malformed.
 */
int stt_session_json_parse_error(char const* json, stt_session_error_t* error);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
