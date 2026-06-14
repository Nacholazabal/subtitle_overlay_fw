/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file stt_event_rx.h
/// @brief Nonblocking TCP NDJSON receiver for STT transcript events
///

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

// === Public data type declarations =============================================================================== //

typedef struct
{
    char host[STT_EVENT_RX_HOST_MAX_LEN];
    uint32_t port;
} stt_event_rx_config_t;

typedef struct
{
    stt_event_rx_config_t config;
    char line[STT_EVENT_RX_LINE_MAX];
    size_t line_used;
    int server_fd;
    int client_fd;
    uint8_t initialized;
    uint8_t client_connected;
    uint8_t discarding_line;
} stt_event_rx_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

void stt_event_rx_default_config(stt_event_rx_config_t* config);
int stt_event_rx_init(stt_event_rx_t* rx, stt_event_rx_config_t const* config);
int stt_event_rx_poll(stt_event_rx_t* rx,
                      subtitle_text_evt_t* events,
                      uint32_t max_events,
                      uint32_t* event_count);
void stt_event_rx_cleanup(stt_event_rx_t* rx);
int stt_event_rx_parse_line(char const* line, subtitle_text_evt_t* event);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
