/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_event_rx.h
/// @brief Nonblocking TCP NDJSON receiver for STT transcript events
///
/// Wire protocol contract (must match the PC-side sender):
///  * Transport: one JSON object per line, newline-delimited (NDJSON), over TCP.
///  * Encoding: UTF-8 **literal** bytes. The sender MUST emit non-ASCII text as
///    raw UTF-8 (Python: `json.dumps(..., ensure_ascii=False)`). `\uXXXX`
///    escapes are NOT decoded and are rejected; only `\" \\ \/ \b \f \n \r \t`
///    are accepted (whitespace escapes collapse to a space).
///  * Field length: `text` is truncated to fit `SUBTITLE_TEXT_MAX_LEN`; the
///    truncation always lands on a UTF-8 code-point boundary (never a split
///    multi-byte sequence).
///  * Delivery/ACK: a transcript whose `seq` is parsed is considered consumed.
///    If it is later dropped (pool/queue full) the receiver reports a definitive
///    drop for that `seq`; the same `seq` is NOT retransmitted within a session
///    (a later resend of an old `seq` is rejected as stale). Drops are final by
///    design.

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

#include "app.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define STT_EVENT_RX_DEFAULT_HOST        "0.0.0.0"
#define STT_EVENT_RX_DEFAULT_PORT        (5001U)
#define STT_EVENT_RX_HOST_MAX_LEN        (64U)
#define STT_EVENT_RX_LINE_MAX            (512U)
#define STT_EVENT_RX_MAX_BYTES_PER_POLL  (512U)
#define STT_EVENT_RX_MAX_EVENTS_PER_POLL (4U)
#define STT_EVENT_RX_TX_QUEUE_DEPTH      (16U)
#define STT_EVENT_RX_RESPONSE_MAX        (160U)

// === Public data type declarations =============================================================================== //

typedef struct
{
    char host[STT_EVENT_RX_HOST_MAX_LEN];
    uint32_t port;
} stt_event_rx_config_t;

typedef enum
{
    STT_EVENT_RX_DELIVERY_ACCEPTED = 0,
    STT_EVENT_RX_DELIVERY_DROPPED_EVENT_POOL,
    STT_EVENT_RX_DELIVERY_DROPPED_SUBTITLE_QUEUE,
} stt_event_rx_delivery_status_t;

typedef struct
{
    char data[STT_EVENT_RX_RESPONSE_MAX];
    uint16_t length;
    uint16_t sent;
} stt_event_rx_response_t;

typedef struct
{
    stt_event_rx_config_t config;
    char line[STT_EVENT_RX_LINE_MAX];
    size_t line_used;
    int server_fd;
    int client_fd;
    stt_event_rx_response_t responses[STT_EVENT_RX_TX_QUEUE_DEPTH];
    uint32_t session_id;
    uint32_t last_seq;
    uint8_t response_head;
    uint8_t response_count;
    uint8_t initialized;
    uint8_t client_connected;
    uint8_t discarding_line;
    uint8_t have_last_seq;
} stt_event_rx_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Populate defaults and valid environment overrides; invalid overrides retain defaults.
void stt_event_rx_default_config(stt_event_rx_config_t* config);

/// @brief Bind the Linux TCP listener and initialize owned receiver state.
/// @return 0 on success or a negative errno-style status.
int stt_event_rx_init(stt_event_rx_t* rx, stt_event_rx_config_t const* config);

/// @brief Perform one bounded, nonblocking socket poll into caller-owned event storage.
int stt_event_rx_poll(stt_event_rx_t* rx,
                      subtitle_text_evt_t* events,
                      uint32_t max_events,
                      uint32_t* event_count);

/// @brief Queue the delivery result and attempt an immediate nonblocking flush.
/// Unsent bytes remain queued and are retried by stt_event_rx_poll().
/// @return 0 on success, -EAGAIN if the nonblocking response queue is full, or -APP_ESTATE.
int stt_event_rx_report_delivery(stt_event_rx_t* rx,
                                 uint32_t seq,
                                 stt_event_rx_delivery_status_t status);

/// @brief Close owned Linux file descriptors; safe for null and partially initialized instances.
void stt_event_rx_cleanup(stt_event_rx_t* rx);

/// @brief Parse the project sender's flat top-level NDJSON object; this is not a general JSON API.
int stt_event_rx_parse_line(char const* line, subtitle_text_evt_t* event);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
