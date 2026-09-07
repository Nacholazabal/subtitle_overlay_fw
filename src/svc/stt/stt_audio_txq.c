/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_audio_txq.c
/// @brief Bounded audio transmit queue with drop-oldest-on-full policy
///

// === Headers files inclusions ==================================================================================== //

#include "stt_audio_txq.h"

#include <errno.h>
#include <string.h>
#include <time.h>

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

int stt_audio_txq_init(stt_audio_txq_t* const txq)
{
    pthread_condattr_t cond_attr;

    if (txq == NULL)
    {
        return -EINVAL;
    }

    memset(txq, 0, sizeof(*txq));
    pthread_mutex_init(&txq->lock, NULL);
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&txq->condvar, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
    return 0;
}

void stt_audio_txq_cleanup(stt_audio_txq_t* const txq)
{
    if (txq == NULL)
    {
        return;
    }

    pthread_cond_destroy(&txq->condvar);
    pthread_mutex_destroy(&txq->lock);
    memset(txq, 0, sizeof(*txq));
}

void stt_audio_txq_worker_started(stt_audio_txq_t* const txq)
{
    if (txq == NULL)
    {
        return;
    }

    pthread_mutex_lock(&txq->lock);
    txq->worker_started = 1U;
    pthread_mutex_unlock(&txq->lock);
}

int stt_audio_txq_push(stt_audio_txq_t* const txq,
                       void const* const pcm,
                       size_t const size,
                       uint64_t const timestamp_ns,
                       uint32_t const dropped)
{
    uint32_t tail;

    if ((txq == NULL) || (pcm == NULL) || (size == 0U) || (size > STT_AUDIO_TXQ_MAX_BYTES))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&txq->lock);
    if ((txq->worker_started == 0U) || (txq->stop_requested != 0U))
    {
        pthread_mutex_unlock(&txq->lock);
        return -EAGAIN;
    }
    if (txq->count == STT_AUDIO_TXQ_DEPTH)
    {
        txq->head = (uint8_t)((txq->head + 1U) % STT_AUDIO_TXQ_DEPTH);
        txq->count--;
        txq->dropped_total++;
        txq->chunks_dropped_tx++;
    }

    tail = ((uint32_t)txq->head + (uint32_t)txq->count) % STT_AUDIO_TXQ_DEPTH;
    memcpy(txq->queue[tail].payload, pcm, size);
    txq->queue[tail].size = size;
    txq->queue[tail].timestamp_ns = timestamp_ns;
    txq->queue[tail].dropped = dropped;
    txq->count++;
    pthread_cond_signal(&txq->condvar);
    pthread_mutex_unlock(&txq->lock);
    return 0;
}

int stt_audio_txq_pop(stt_audio_txq_t* const txq, stt_audio_txq_chunk_t* const chunk)
{
    int status = -EAGAIN;

    if ((txq == NULL) || (chunk == NULL))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&txq->lock);
    if (txq->count > 0U)
    {
        *chunk = txq->queue[txq->head];
        chunk->dropped += txq->dropped_total;
        txq->head = (uint8_t)((txq->head + 1U) % STT_AUDIO_TXQ_DEPTH);
        txq->count--;
        status = 0;
    }
    pthread_mutex_unlock(&txq->lock);
    return status;
}

void stt_audio_txq_discard_all(stt_audio_txq_t* const txq)
{
    if (txq == NULL)
    {
        return;
    }

    pthread_mutex_lock(&txq->lock);
    txq->chunks_dropped_tx += txq->count;
    txq->dropped_total += txq->count;
    txq->head = 0U;
    txq->count = 0U;
    pthread_mutex_unlock(&txq->lock);
}

void stt_audio_txq_request_stop(stt_audio_txq_t* const txq)
{
    if (txq == NULL)
    {
        return;
    }

    pthread_mutex_lock(&txq->lock);
    txq->stop_requested = 1U;
    pthread_cond_broadcast(&txq->condvar);
    pthread_mutex_unlock(&txq->lock);
}

uint8_t stt_audio_txq_stop_requested(stt_audio_txq_t* const txq)
{
    uint8_t requested;

    if (txq == NULL)
    {
        return 0U;
    }

    pthread_mutex_lock(&txq->lock);
    requested = txq->stop_requested;
    pthread_mutex_unlock(&txq->lock);
    return requested;
}

void stt_audio_txq_wait(stt_audio_txq_t* const txq, uint32_t const timeout_ms)
{
    struct timespec deadline;

    if (txq == NULL)
    {
        return;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    {
        return;
    }
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    deadline.tv_sec += (time_t)(timeout_ms / 1000U) + (deadline.tv_nsec / 1000000000L);
    deadline.tv_nsec %= 1000000000L;

    pthread_mutex_lock(&txq->lock);
    if ((txq->count == 0U) && (txq->stop_requested == 0U))
    {
        (void)pthread_cond_timedwait(&txq->condvar, &txq->lock, &deadline);
    }
    pthread_mutex_unlock(&txq->lock);
}

uint32_t stt_audio_txq_get_dropped_count(stt_audio_txq_t* const txq)
{
    uint32_t dropped;

    if (txq == NULL)
    {
        return 0U;
    }

    pthread_mutex_lock(&txq->lock);
    dropped = txq->chunks_dropped_tx;
    pthread_mutex_unlock(&txq->lock);
    return dropped;
}

uint32_t stt_audio_txq_get_count(stt_audio_txq_t* const txq)
{
    uint32_t count;

    if (txq == NULL)
    {
        return 0U;
    }

    pthread_mutex_lock(&txq->lock);
    count = (uint32_t)txq->count;
    pthread_mutex_unlock(&txq->lock);
    return count;
}
