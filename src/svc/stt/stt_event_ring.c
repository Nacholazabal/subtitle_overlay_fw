/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_event_ring.c
/// @brief Transcript event ring with shed-oldest-partial-first eviction policy
///

// === Headers files inclusions ==================================================================================== //

#include "stt_event_ring.h"

#include <errno.h>
#include <string.h>

#include "log.h"
#include "stt_transcript_parse.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

int stt_event_ring_init(stt_event_ring_t* const ring)
{
    if (ring == NULL)
    {
        return -EINVAL;
    }

    memset(ring, 0, sizeof(*ring));
    pthread_mutex_init(&ring->lock, NULL);
    return 0;
}

void stt_event_ring_cleanup(stt_event_ring_t* const ring)
{
    if (ring == NULL)
    {
        return;
    }

    pthread_mutex_destroy(&ring->lock);
    memset(ring, 0, sizeof(*ring));
}

int stt_event_ring_push(stt_event_ring_t* const ring, char const* const line, size_t const length)
{
    uint8_t const is_final = (strstr(line, "\"is_final\":true") != NULL) ? 1U : 0U;
    uint32_t tail;

    if (ring == NULL)
    {
        return -EINVAL;
    }
    if (length >= STT_EVENT_RING_LINE_MAX)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&ring->lock);

    if (ring->count == STT_EVENT_RING_DEPTH)
    {
        uint32_t victim = STT_EVENT_RING_DEPTH;
        uint32_t i;

        for (i = 0U; i < STT_EVENT_RING_DEPTH; i++)
        {
            uint32_t const slot = ((uint32_t)ring->head + i) % STT_EVENT_RING_DEPTH;

            if (ring->ring[slot].is_final == 0U)
            {
                victim = slot;
                break;
            }
        }

        if (victim == STT_EVENT_RING_DEPTH)
        {
            victim = ring->head;
        }

        while (victim != ring->head)
        {
            uint32_t const previous = (victim + STT_EVENT_RING_DEPTH - 1U) % STT_EVENT_RING_DEPTH;

            ring->ring[victim] = ring->ring[previous];
            victim = previous;
        }
        ring->head = (uint8_t)((ring->head + 1U) % STT_EVENT_RING_DEPTH);
        ring->count--;
        ring->events_dropped_ring++;
    }

    tail = ((uint32_t)ring->head + (uint32_t)ring->count) % STT_EVENT_RING_DEPTH;
    memcpy(ring->ring[tail].line, line, length);
    ring->ring[tail].line[length] = '\0';
    ring->ring[tail].length = (uint16_t)length;
    ring->ring[tail].is_final = is_final;
    ring->ring[tail].session_generation = ring->session_generation;
    ring->count++;

    ring->transcripts_received++;
    if (is_final != 0U)
    {
        ring->transcripts_final++;
    }
    else
    {
        ring->transcripts_partial++;
    }

    pthread_mutex_unlock(&ring->lock);
    return 0;
}

int stt_event_ring_drain(stt_event_ring_t* const ring,
                         subtitle_text_evt_t* const events,
                         uint32_t const max_events,
                         uint32_t* const event_count)
{
    stt_event_ring_entry_t pending[STT_EVENT_RING_DEPTH];
    uint32_t taken = 0U;
    uint32_t i;

    if ((ring == NULL) || (events == NULL) || (max_events == 0U) || (event_count == NULL))
    {
        return -EINVAL;
    }

    *event_count = 0U;

    pthread_mutex_lock(&ring->lock);
    while ((ring->count > 0U) && (taken < max_events) && (taken < STT_EVENT_RING_DEPTH))
    {
        pending[taken] = ring->ring[ring->head];
        ring->head = (uint8_t)((ring->head + 1U) % STT_EVENT_RING_DEPTH);
        ring->count--;
        taken++;
    }
    pthread_mutex_unlock(&ring->lock);

    for (i = 0U; i < taken; i++)
    {
        subtitle_text_evt_t* const target = &events[*event_count];

        if (stt_transcript_parse_line(pending[i].line, target) != 0)
        {
            continue;
        }
        pthread_mutex_lock(&ring->lock);
        if (pending[i].session_generation != ring->session_generation)
        {
            ring->events_dropped_ring++;
            pthread_mutex_unlock(&ring->lock);
            continue;
        }
        if (ring->last_event_generation != pending[i].session_generation)
        {
            ring->last_event_generation = pending[i].session_generation;
            ring->have_last_event_seq = 0U;
            ring->last_event_seq = 0U;
        }
        if ((ring->have_last_event_seq != 0U) && (target->seq <= ring->last_event_seq))
        {
            ring->events_rejected_old_seq++;
            {
                uint32_t const last = ring->last_event_seq;

                pthread_mutex_unlock(&ring->lock);
                LOG_WARNING("stt-event-ring: rejecting out-of-order transcript seq=%lu last=%lu",
                            (unsigned long)target->seq, (unsigned long)last);
            }
            continue;
        }

        ring->last_event_seq = target->seq;
        ring->have_last_event_seq = 1U;
        pthread_mutex_unlock(&ring->lock);
        (*event_count)++;
    }

    return 0;
}

uint32_t stt_event_ring_invalidate_session(stt_event_ring_t* const ring)
{
    uint32_t discarded;

    if (ring == NULL)
    {
        return 0U;
    }

    pthread_mutex_lock(&ring->lock);
    ring->session_generation++;
    ring->have_last_event_seq = 0U;
    ring->last_event_seq = 0U;
    ring->last_event_generation = ring->session_generation;
    discarded = (uint32_t)ring->count;
    ring->events_dropped_ring += discarded;
    ring->head = 0U;
    ring->count = 0U;
    pthread_mutex_unlock(&ring->lock);
    return discarded;
}

uint32_t stt_event_ring_get_generation(stt_event_ring_t* const ring)
{
    uint32_t generation;

    if (ring == NULL)
    {
        return 0U;
    }

    pthread_mutex_lock(&ring->lock);
    generation = ring->session_generation;
    pthread_mutex_unlock(&ring->lock);
    return generation;
}

uint32_t stt_event_ring_get_dropped_count(stt_event_ring_t* const ring)
{
    uint32_t dropped;

    if (ring == NULL)
    {
        return 0U;
    }

    pthread_mutex_lock(&ring->lock);
    dropped = ring->events_dropped_ring;
    pthread_mutex_unlock(&ring->lock);
    return dropped;
}

uint32_t stt_event_ring_get_rejected_count(stt_event_ring_t* const ring)
{
    uint32_t rejected;

    if (ring == NULL)
    {
        return 0U;
    }

    pthread_mutex_lock(&ring->lock);
    rejected = ring->events_rejected_old_seq;
    pthread_mutex_unlock(&ring->lock);
    return rejected;
}
