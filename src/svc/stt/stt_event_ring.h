/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_event_ring.h
/// @brief Transcript event ring with shed-oldest-partial-first eviction policy
///
/// Threading: the ring owns its mutex. The network worker pushes parsed
/// transcript lines; the QP/C thread drains them without blocking on I/O.
///
/// Eviction policy (shed-oldest-partial-first): when full and a new event
/// arrives, the oldest *partial* transcript is shed first, since it will be
/// superseded by the next partial anyway. If all buffered events are finals,
/// the oldest final is dropped instead.
///
/// Deduplication: per-session generation and sequence tracking. A session
/// generation bump invalidates all buffered events from the prior session,
/// and resets the sequence counter so seq=0 can arrive again. Out-of-order
/// and duplicate sequences are rejected.
///

// === Headers files inclusions ==================================================================================== //

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define STT_EVENT_RING_DEPTH (8U)
#define STT_EVENT_RING_LINE_MAX (1280U)

// === Public data type declarations =============================================================================== //

/// One buffered transcript line waiting for the QP/C thread.
typedef struct
{
    char line[STT_EVENT_RING_LINE_MAX];
    uint16_t length;
    uint8_t is_final;
    uint32_t session_generation;
} stt_event_ring_entry_t;

/// Transcript event ring instance. Fields are mutex-guarded.
typedef struct
{
    pthread_mutex_t lock;
    stt_event_ring_entry_t ring[STT_EVENT_RING_DEPTH];
    uint8_t head;
    uint8_t count;
    uint32_t session_generation;
    uint32_t last_event_generation;
    uint32_t last_event_seq;
    uint8_t have_last_event_seq;
    uint32_t events_dropped_ring;
    uint32_t events_rejected_old_seq;
    uint32_t transcripts_received;
    uint32_t transcripts_partial;
    uint32_t transcripts_final;
} stt_event_ring_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Initialize the event ring.
 * @param ring Ring instance.
 * @return 0 on success, or a negative errno-style value.
 */
int stt_event_ring_init(stt_event_ring_t* ring);

/**
 * @brief Release ring resources.
 * @param ring Ring instance.
 */
void stt_event_ring_cleanup(stt_event_ring_t* ring);

/**
 * @brief Push one transcript line, shedding oldest partial if full.
 *
 * When full, the oldest *partial* is shed first (partials supersede each
 * other). If all buffered events are finals, the oldest final is dropped.
 *
 * @param ring Ring instance.
 * @param line Transcript line (JSON text).
 * @param length Line length.
 * @return 0 on success, or -EINVAL when line is too long.
 */
int stt_event_ring_push(stt_event_ring_t* ring, char const* line, size_t length);

/**
 * @brief Drain buffered transcripts into caller-owned events.
 *
 * Copies up to max_events from the ring, parses each, and applies generation
 * and sequence deduplication. Events from a dead session (stale generation)
 * are discarded. Out-of-order and duplicate sequences are rejected.
 *
 * @param ring Ring instance.
 * @param events Destination array.
 * @param max_events Destination capacity.
 * @param event_count Number of events written.
 * @return 0 on success, or a negative errno-style value.
 */
int stt_event_ring_drain(stt_event_ring_t* ring,
                         subtitle_text_evt_t* events,
                         uint32_t max_events,
                         uint32_t* event_count);

/**
 * @brief Invalidate all buffered events and reset sequence tracking.
 *
 * Call this on session restart. The generation bump invalidates copied events
 * from the old session, and the sequence reset lets seq=0 arrive again.
 *
 * @param ring Ring instance.
 * @return Number of events discarded.
 */
uint32_t stt_event_ring_invalidate_session(stt_event_ring_t* ring);

/**
 * @brief Get current session generation.
 * @param ring Ring instance.
 * @return Generation counter.
 */
uint32_t stt_event_ring_get_generation(stt_event_ring_t* ring);

/**
 * @brief Get count of events dropped due to ring overflow.
 * @param ring Ring instance.
 * @return Cumulative drop count.
 */
uint32_t stt_event_ring_get_dropped_count(stt_event_ring_t* ring);

/**
 * @brief Get count of events rejected due to old/duplicate sequence.
 * @param ring Ring instance.
 * @return Cumulative rejection count.
 */
uint32_t stt_event_ring_get_rejected_count(stt_event_ring_t* ring);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif

/**
 * @brief Get current event count in ring (for stats tracking).
 * @param ring Ring instance.
 * @return Current count, or 0 if ring is NULL.
 */
uint32_t stt_event_ring_get_count(stt_event_ring_t const* ring);

