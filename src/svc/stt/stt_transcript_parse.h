/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_transcript_parse.h
/// @brief Parser for the streaming server's transcript messages
///
/// Message contract (`server/runtime/nemotron.py`, `TranscriptAdapter._emit`):
///  * Encoding: UTF-8 **literal** bytes. The sender emits non-ASCII text raw
///    (Python: `json.dumps(..., ensure_ascii=False)`). `\uXXXX` escapes are NOT
///    decoded and are rejected; only `\" \\ \/ \b \f \n \r \t` are accepted,
///    and the whitespace escapes collapse to a space.
///  * Required fields: `seq`, `start_sec`, `end_sec`, `text`, plus finality.
///  * `type`: `"final"`/`"partial"` carry finality (legacy sender dialect);
///    `"transcript"` is the WebSocket message discriminator and carries none,
///    so those messages must also supply `is_final`. Any other value is
///    rejected. When both carry finality they must agree.
///  * Unknown fields are skipped, including arrays and nested objects — the
///    server sends `"att_context_size":[56,6]` and more that the firmware
///    ignores. Nesting deeper than ::STT_JSON_MAX_DEPTH is rejected.
///  * `text` is truncated to fit ::SUBTITLE_TEXT_MAX_LEN, always on a UTF-8
///    code-point boundary, never splitting a multi-byte sequence.
///
/// This is not a general JSON API: it parses one flat top-level object.
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "app.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

/// @brief What happened to a transcript after the STT AO forwarded it.
///
/// The PC bridge used to receive these as `transcript_ack` messages. With the
/// board talking to the server directly there is no peer to acknowledge to, so
/// "delivered" now means "posted to the subtitle active object" and these
/// outcomes survive as counters and log lines instead.
typedef enum
{
    STT_EVENT_RX_DELIVERY_ACCEPTED = 0,
    STT_EVENT_RX_DELIVERY_DROPPED_EVENT_POOL,
    STT_EVENT_RX_DELIVERY_DROPPED_SUBTITLE_QUEUE,
} stt_event_rx_delivery_status_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Parse one transcript message into a subtitle text event.
 * @param line NUL-terminated JSON object.
 * @param event Parsed payload destination, zeroed before use.
 * @return 0 on success, -EINVAL when the message violates the contract above,
 *         or -ERANGE when a numeric field is out of range.
 */
int stt_transcript_parse_line(char const* line, subtitle_text_evt_t* event);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
