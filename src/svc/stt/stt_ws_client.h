/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_ws_client.h
/// @brief Outbound WebSocket session carrying audio out and transcripts back
///
/// Ownership and threading:
///  * every network call runs on the STT subsystem's worker thread, which is
///    allowed to block without stalling the cooperative QP/C scheduler;
///  * that worker is the sole owner of the connection handle, so no lock guards
///    the socket;
///  * received transcript lines cross into the QP/C thread through a bounded
///    ring, and ::stt_ws_client_poll_events is the only function `SttAO` calls.
///    It takes a mutex, copies and returns; it performs no I/O and never blocks
///    on the network, so a dead link cannot stall video or subtitles.
///
/// Failure policy: every error path leads to ::STT_WS_STATE_BACKOFF and a
/// retry. Losing the network is normal operation, not a firmware fault, so the
/// client never reports a fatal error upward and never needs a restart.
///

// === Headers files inclusions ==================================================================================== //

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "net_tls.h"
#include "stt_audio_txq.h"
#include "stt_event_ring.h"
#include "stt_ws_config.h"
#include "stt_transcript_parse.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

/// Largest server frame accepted before the session is treated as hostile.
#define STT_WS_RX_MAX_PAYLOAD (8192U)

// === Public data type declarations =============================================================================== //

/// @brief Connection lifecycle; every failure funnels into ::STT_WS_STATE_BACKOFF.
typedef enum
{
    STT_WS_STATE_IDLE = 0,
    STT_WS_STATE_WAIT_CLOCK,
    STT_WS_STATE_CONNECTING,
    STT_WS_STATE_HANDSHAKING,
    STT_WS_STATE_STARTING,
    STT_WS_STATE_READY,
    STT_WS_STATE_BACKOFF,
} stt_ws_state_e;

/// @brief Counters that replace what the PC bridge used to record.
typedef struct
{
    uint32_t chunks_sent;
    uint32_t chunks_dropped_tx;
    uint64_t bytes_sent;
    uint32_t sessions;
    uint32_t reconnects;
    uint32_t clock_deferrals;
    uint32_t connect_failures;
    uint32_t tls_failures;
    uint32_t handshake_failures;
    uint32_t busy_rejections;
    uint32_t protocol_errors;
    uint32_t transcripts_received;
    uint32_t transcripts_partial;
    uint32_t transcripts_final;
    uint32_t events_dropped_ring;
    uint32_t events_rejected_old_seq;
    uint32_t deliveries_accepted;
    uint32_t deliveries_dropped_pool;
    uint32_t deliveries_dropped_queue;
} stt_ws_client_stats_t;

/// @brief Client instance; fields are private to the implementation.
typedef struct
{
    stt_ws_config_t config;
    stt_ws_client_stats_t stats;
    stt_audio_txq_t audio_txq;
    stt_event_ring_t event_ring;

    net_tls_t* conn;
    stt_ws_state_e state;

    pthread_mutex_t lock;
    pthread_t worker_thread;
    pthread_cond_t worker_cond;

    uint8_t rx[STT_WS_RX_MAX_PAYLOAD + 64U];
    size_t rx_used;

    char msg[STT_EVENT_RING_LINE_MAX];
    size_t msg_used;
    uint8_t msg_active;
    uint8_t msg_overflow;

    uint32_t backoff_ms;
    uint64_t next_attempt_ms;
    uint64_t last_rx_ms;
    uint64_t last_tx_ms;
    uint64_t session_started_ms;

    uint32_t audio_seq;
    uint8_t worker_done;
    uint8_t worker_started;
    uint8_t stop_requested;
    uint8_t initialized;
} stt_ws_client_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //


/**
 * @brief The one client instance shared by the audio sender and by `SttAO`.
 *
 * There is exactly one outbound session. The USB-audio capture thread submits
 * chunks to its bounded queue, its owned worker performs all network I/O, and
 * the QP/C thread drains transcripts. Initialization is lazy and startup-order
 * independent.
 *
 * @return The shared client, or NULL when configuration failed (typically a
 *         missing `SUBTITLE_STT_WS_URL`), in which case the STT link stays down.
 */
stt_ws_client_t* stt_ws_client_shared(void);

/**
 * @brief Initialize the client without touching the network.
 * @param client Client instance.
 * @param config Runtime configuration.
 * @return 0 on success, or a negative errno-style value.
 */
int stt_ws_client_init(stt_ws_client_t* client, stt_ws_config_t const* config);

/// @brief Start the owned network worker; no network I/O runs on the caller.
int stt_ws_client_start(stt_ws_client_t* client);

/// @brief Queue recent PCM for the network worker without blocking the capture thread.
int stt_ws_client_submit_audio(stt_ws_client_t* client,
                               void const* pcm,
                               size_t size,
                               uint64_t timestamp_ns,
                               uint32_t dropped);

/// @brief Request worker shutdown and wake it; always nonblocking.
void stt_ws_client_request_stop(stt_ws_client_t* client);

/// @brief Return nonzero after the worker has closed the WebSocket and exited.
uint8_t stt_ws_client_stop_complete(stt_ws_client_t* client);

/// @brief Join an already-stopped worker. Returns -EAGAIN while it is still live.
int stt_ws_client_finish_stop(stt_ws_client_t* client);

/**
 * @brief Drain buffered transcripts into caller-owned events. Never blocks.
 *
 * Out-of-order and duplicate sequences are rejected here, and every new session
 * accepts `seq` 0 again because the server restarts its counter per session.
 *
 * @param client Client instance.
 * @param events Destination array.
 * @param max_events Destination capacity.
 * @param event_count Number of events written.
 * @return 0 on success, or a negative errno-style value.
 */
int stt_ws_client_poll_events(stt_ws_client_t* client,
                              subtitle_text_evt_t* events,
                              uint32_t max_events,
                              uint32_t* event_count);

/**
 * @brief Record what happened to a transcript after `SttAO` forwarded it.
 *
 * With the PC bridge gone there is nobody to acknowledge to, so "delivered"
 * now means "posted to the subtitle active object" and the three outcomes are
 * kept as counters instead of being sent back over the wire.
 *
 * @param client Client instance.
 * @param status Delivery outcome.
 * @return None.
 */
void stt_ws_client_report_delivery(stt_ws_client_t* client, stt_transcript_delivery_status_t status);

/// @brief Copy a consistent snapshot of the counters.
void stt_ws_client_stats(stt_ws_client_t* client, stt_ws_client_stats_t* stats);

/// @brief Current connection state, for logging.
stt_ws_state_e stt_ws_client_state(stt_ws_client_t* client);

/// @brief Human-readable name of @p state.
char const* stt_ws_client_state_name(stt_ws_state_e state);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
