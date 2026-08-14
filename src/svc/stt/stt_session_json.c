/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_session_json.c
/// @brief Session control messages of the streaming STT WebSocket protocol
///

// === Headers files inclusions ==================================================================================== //

#include "stt_session_json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "stt_json.h"

// === Macros definitions ========================================================================================== //

#define STT_SESSION_KEY_MAX  (32U)
#define STT_SESSION_TYPE_MAX (24U)

// === Private data type declarations ============================================================================== //

/// @brief Callback invoked for every top-level key of a scanned object.
/// Returns 0 when it consumed the value, -ENOENT to let the walker skip it, or
/// a negative errno-style value to abort the scan.
typedef int (*field_fn)(char const* key, char const** cursor, void* ctx);

typedef struct
{
    stt_session_msg_e type;
    uint8_t seen;
} type_scan_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static int walk_object(char const* json, field_fn on_field, void* ctx);
static int copy_raw_value(char const** cursor, char* dst, size_t dst_size, uint8_t* truncated);
static stt_session_msg_e type_from_name(char const* name);
static int on_type_field(char const* key, char const** cursor, void* ctx);
static int on_ready_field(char const* key, char const** cursor, void* ctx);
static int on_error_field(char const* key, char const** cursor, void* ctx);
static int append_u32_field(char* out,
                            size_t out_size,
                            size_t used,
                            char const** separator,
                            char const* key,
                            uint32_t value);
static uint8_t language_tag_is_safe(char const* tag);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Scan a flat top-level JSON object, dispatching each key to @p on_field.
 * @param json NUL-terminated JSON object.
 * @param on_field Field callback.
 * @param ctx Opaque context handed to @p on_field.
 * @return 0 on success, or a negative errno-style value.
 */
static int walk_object(char const* json, field_fn const on_field, void* const ctx)
{
    char const* cursor = json;
    char key[STT_SESSION_KEY_MAX];
    uint8_t done = 0U;

    if ((json == NULL) || (on_field == NULL))
    {
        return -EINVAL;
    }

    stt_json_skip_whitespace(&cursor);
    if (*cursor != '{')
    {
        return -EPROTO;
    }
    cursor++;

    stt_json_skip_whitespace(&cursor);
    if (*cursor == '}')
    {
        return 0;
    }

    while (done == 0U)
    {
        uint8_t key_truncated = 0U;
        int status;

        stt_json_skip_whitespace(&cursor);
        status = stt_json_parse_string(&cursor, key, sizeof(key), 0U, &key_truncated);
        if (status != 0)
        {
            return -EPROTO;
        }

        stt_json_skip_whitespace(&cursor);
        if (*cursor != ':')
        {
            return -EPROTO;
        }
        cursor++;
        stt_json_skip_whitespace(&cursor);

        // A truncated key can never equal one we care about, so skip its value.
        status = (key_truncated != 0U) ? -ENOENT : on_field(key, &cursor, ctx);
        if (status == -ENOENT)
        {
            status = (stt_json_skip_value(&cursor) == 0) ? 0 : -EPROTO;
        }
        if (status != 0)
        {
            return status;
        }

        stt_json_skip_whitespace(&cursor);
        if (*cursor == ',')
        {
            cursor++;
        }
        else if (*cursor == '}')
        {
            cursor++;
            done = 1U;
        }
        else
        {
            return -EPROTO;
        }
    }

    stt_json_skip_whitespace(&cursor);
    return (*cursor == '\0') ? 0 : -EPROTO;
}

/**
 * @brief Copy the raw JSON text of one value, then advance past it.
 * @param cursor Cursor positioned on the value.
 * @param dst Destination, NUL-terminated on return.
 * @param dst_size Destination capacity.
 * @param truncated Set to 1 when the value did not fit.
 * @return 0 on success, or -EPROTO when the value is malformed.
 */
static int copy_raw_value(char const** const cursor,
                          char* const dst,
                          size_t dst_size,
                          uint8_t* const truncated)
{
    char const* const start = *cursor;
    size_t span;

    // A zero capacity would underflow the truncation clamp below.
    if ((dst == NULL) || (dst_size == 0U))
    {
        return -EINVAL;
    }
    if (stt_json_skip_value(cursor) != 0)
    {
        return -EPROTO;
    }

    span = (size_t)(*cursor - start);
    if (span >= dst_size)
    {
        span = dst_size - 1U;
        if (truncated != NULL)
        {
            *truncated = 1U;
        }
    }
    memcpy(dst, start, span);
    dst[span] = '\0';
    return 0;
}

/** @brief Map a `type` string to its message kind. */
static stt_session_msg_e type_from_name(char const* const name)
{
    stt_session_msg_e type = STT_SESSION_MSG_UNKNOWN;

    if (strcmp(name, "session_ready") == 0)
    {
        type = STT_SESSION_MSG_SESSION_READY;
    }
    else if (strcmp(name, "transcript") == 0)
    {
        type = STT_SESSION_MSG_TRANSCRIPT;
    }
    else if (strcmp(name, "session_summary") == 0)
    {
        type = STT_SESSION_MSG_SESSION_SUMMARY;
    }
    else if (strcmp(name, "error") == 0)
    {
        type = STT_SESSION_MSG_ERROR;
    }
    else if (strcmp(name, "pong") == 0)
    {
        type = STT_SESSION_MSG_PONG;
    }

    return type;
}

/** @brief Collect the `type` field only. */
static int on_type_field(char const* const key, char const** const cursor, void* const ctx)
{
    type_scan_t* const scan = (type_scan_t*)ctx;
    char name[STT_SESSION_TYPE_MAX];

    if (strcmp(key, "type") != 0)
    {
        return -ENOENT;
    }
    if (stt_json_parse_string(cursor, name, sizeof(name), 1U, NULL) != 0)
    {
        return -EPROTO;
    }

    scan->type = type_from_name(name);
    scan->seen = 1U;
    return 0;
}

/** @brief Collect the fields of a `session_ready` message. */
static int on_ready_field(char const* const key, char const** const cursor, void* const ctx)
{
    stt_session_ready_t* const ready = (stt_session_ready_t*)ctx;

    if (strcmp(key, "version") == 0)
    {
        return (stt_json_parse_u32(cursor, &ready->version) == 0) ? 0 : -EPROTO;
    }
    if (strcmp(key, "sample_rate_hz") == 0)
    {
        return (stt_json_parse_u32(cursor, &ready->sample_rate_hz) == 0) ? 0 : -EPROTO;
    }
    if (strcmp(key, "run_engine") == 0)
    {
        return (stt_json_parse_string(cursor, ready->run_engine, sizeof(ready->run_engine), 0U,
                                      NULL)
                == 0)
                   ? 0
                   : -EPROTO;
    }
    if (strcmp(key, "run_config") == 0)
    {
        // Kept verbatim: this is the effective configuration the run is logged with.
        return copy_raw_value(cursor, ready->run_config, sizeof(ready->run_config),
                              &ready->run_config_truncated);
    }

    return -ENOENT;
}

/** @brief Collect the fields of an `error` message. */
static int on_error_field(char const* const key, char const** const cursor, void* const ctx)
{
    stt_session_error_t* const error = (stt_session_error_t*)ctx;

    if (strcmp(key, "message") == 0)
    {
        return (stt_json_parse_string(cursor, error->message, sizeof(error->message), 0U, NULL) == 0)
                   ? 0
                   : -EPROTO;
    }
    if (strcmp(key, "busy") == 0)
    {
        return (stt_json_parse_bool(cursor, &error->busy) == 0) ? 0 : -EPROTO;
    }

    return -ENOENT;
}

/**
 * @brief Append `"key":value` when @p value is set, skipping zero.
 * @param separator In/out: emitted before the field, then set to ",".
 * @return New used length, or a negative errno-style value.
 */
static int append_u32_field(char* const out,
                            size_t out_size,
                            size_t used,
                            char const** const separator,
                            char const* const key,
                            uint32_t value)
{
    int written;

    if (value == 0U)
    {
        return (int)used;
    }

    written = snprintf(&out[used], out_size - used, "%s\"%s\":%lu", *separator, key,
                       (unsigned long)value);
    if ((written < 0) || ((size_t)written >= (out_size - used)))
    {
        return -ENOBUFS;
    }

    *separator = ",";
    return (int)(used + (size_t)written);
}

/**
 * @brief Report whether a language tag is safe to embed unescaped.
 *
 * The value reaches us from the environment, so it must not be able to close
 * the JSON string and inject fields into `backend_config`.
 */
static uint8_t language_tag_is_safe(char const* const tag)
{
    size_t i;

    for (i = 0U; tag[i] != '\0'; i++)
    {
        char const ch = tag[i];
        uint8_t const allowed = (uint8_t)(((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z'))
                                          || ((ch >= '0') && (ch <= '9')) || (ch == '-')
                                          || (ch == '_'));

        if (allowed == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Serialize the `session_start` opening message.
 * @param out Destination buffer.
 * @param out_size Destination capacity.
 * @param start Stream description and backend overrides.
 * @return Message length in bytes, or a negative errno-style value.
 */
int stt_session_json_build_start(char* const out, size_t out_size, stt_session_start_t const* start)
{
    int used;
    int written;

    if ((out == NULL) || (start == NULL) || (out_size == 0U))
    {
        return -EINVAL;
    }
    // The server validates these too; failing here keeps a bad session from
    // costing a connection attempt and a GPU slot.
    if ((start->sample_rate_hz == 0U) || (start->channels != 1U)
        || (start->format != STT_SESSION_FORMAT_S16_LE) || (start->bytes_per_chunk == 0U))
    {
        return -EINVAL;
    }

    written = snprintf(out,
                       out_size,
                       "{\"type\":\"session_start\",\"version\":%u,\"sample_rate_hz\":%lu,"
                       "\"channels\":%lu,\"format\":%lu,\"chunk_ms\":%lu,"
                       "\"samples_per_chunk\":%lu,\"bytes_per_chunk\":%lu",
                       (unsigned)STT_SESSION_PROTOCOL_VERSION,
                       (unsigned long)start->sample_rate_hz,
                       (unsigned long)start->channels,
                       (unsigned long)start->format,
                       (unsigned long)start->chunk_ms,
                       (unsigned long)start->samples_per_chunk,
                       (unsigned long)start->bytes_per_chunk);
    if ((written < 0) || ((size_t)written >= out_size))
    {
        return -ENOBUFS;
    }
    used = written;

    if ((start->latency_ms != 0U) || (start->stop_history_eou_ms != 0U)
        || (start->residue_tokens_at_end != 0U) || (start->target_lang[0] != '\0'))
    {
        char const* separator = "";

        if ((start->target_lang[0] != '\0') && (language_tag_is_safe(start->target_lang) == 0U))
        {
            return -EINVAL;
        }

        written = snprintf(&out[used], out_size - (size_t)used, ",\"backend_config\":{");
        if ((written < 0) || ((size_t)written >= (out_size - (size_t)used)))
        {
            return -ENOBUFS;
        }
        used += written;

        if (start->target_lang[0] != '\0')
        {
            written = snprintf(&out[used], out_size - (size_t)used, "\"target_lang\":\"%s\"",
                               start->target_lang);
            if ((written < 0) || ((size_t)written >= (out_size - (size_t)used)))
            {
                return -ENOBUFS;
            }
            used += written;
            separator = ",";
        }

        used = append_u32_field(out, out_size, (size_t)used, &separator, "latency_ms",
                                start->latency_ms);
        if (used < 0)
        {
            return used;
        }
        used = append_u32_field(out, out_size, (size_t)used, &separator, "stop_history_eou_ms",
                                start->stop_history_eou_ms);
        if (used < 0)
        {
            return used;
        }
        used = append_u32_field(out, out_size, (size_t)used, &separator, "residue_tokens_at_end",
                                start->residue_tokens_at_end);
        if (used < 0)
        {
            return used;
        }

        written = snprintf(&out[used], out_size - (size_t)used, "}");
        if ((written < 0) || ((size_t)written >= (out_size - (size_t)used)))
        {
            return -ENOBUFS;
        }
        used += written;
    }

    written = snprintf(&out[used], out_size - (size_t)used, "}");
    if ((written < 0) || ((size_t)written >= (out_size - (size_t)used)))
    {
        return -ENOBUFS;
    }

    return used + written;
}

/**
 * @brief Identify a server message by its top-level `type` field.
 * @param json NUL-terminated JSON object.
 * @return The message kind, or ::STT_SESSION_MSG_UNKNOWN.
 */
stt_session_msg_e stt_session_json_message_type(char const* const json)
{
    type_scan_t scan;

    memset(&scan, 0, sizeof(scan));
    if (walk_object(json, on_type_field, &scan) != 0)
    {
        return STT_SESSION_MSG_UNKNOWN;
    }

    return (scan.seen != 0U) ? scan.type : STT_SESSION_MSG_UNKNOWN;
}

/**
 * @brief Parse and validate a `session_ready` message.
 * @param json NUL-terminated JSON object.
 * @param ready Destination.
 * @return 0 on success, or -EPROTO.
 */
int stt_session_json_parse_ready(char const* const json, stt_session_ready_t* const ready)
{
    if ((json == NULL) || (ready == NULL))
    {
        return -EINVAL;
    }

    memset(ready, 0, sizeof(*ready));
    if (stt_session_json_message_type(json) != STT_SESSION_MSG_SESSION_READY)
    {
        return -EPROTO;
    }
    if (walk_object(json, on_ready_field, ready) != 0)
    {
        return -EPROTO;
    }
    // Refusing an unknown version is what keeps a newer server from silently
    // changing the audio framing under a firmware that cannot follow it.
    if (ready->version != STT_SESSION_PROTOCOL_VERSION)
    {
        return -EPROTO;
    }

    return 0;
}

/**
 * @brief Parse an `error` message for logging and retry classification.
 * @param json NUL-terminated JSON object.
 * @param error Destination.
 * @return 0 on success, or -EPROTO.
 */
int stt_session_json_parse_error(char const* const json, stt_session_error_t* const error)
{
    if ((json == NULL) || (error == NULL))
    {
        return -EINVAL;
    }

    memset(error, 0, sizeof(*error));
    if (stt_session_json_message_type(json) != STT_SESSION_MSG_ERROR)
    {
        return -EPROTO;
    }

    return (walk_object(json, on_error_field, error) == 0) ? 0 : -EPROTO;
}

// === End of documentation ======================================================================================== //
