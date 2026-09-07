/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_audio_txq.h
/// @brief Bounded audio transmit queue with drop-oldest-on-full policy
///
/// Threading: the queue owns its mutex and condvar. Multiple threads may push
/// (typically the ALSA capture thread); one worker thread pops. Push never
/// blocks the caller — when full, the oldest chunk is shed to make room.
///

// === Headers files inclusions ==================================================================================== //

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define STT_AUDIO_TXQ_DEPTH (16U)
#define STT_AUDIO_TXQ_MAX_BYTES (2048U)

// === Public data type declarations =============================================================================== //

/// One audio chunk queued for transmission.
typedef struct
{
    uint8_t payload[STT_AUDIO_TXQ_MAX_BYTES];
    size_t size;
    uint64_t timestamp_ns;
    uint32_t dropped;
} stt_audio_txq_chunk_t;

/// Audio transmit queue instance. Fields are private.
typedef struct
{
    pthread_mutex_t lock;
    pthread_cond_t condvar;
    stt_audio_txq_chunk_t queue[STT_AUDIO_TXQ_DEPTH];
    uint8_t head;
    uint8_t count;
    uint32_t dropped_total;
    uint32_t chunks_dropped_tx;
    uint8_t worker_started;
    uint8_t stop_requested;
} stt_audio_txq_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Initialize the audio queue.
 * @param txq Queue instance.
 * @return 0 on success, or a negative errno-style value.
 */
int stt_audio_txq_init(stt_audio_txq_t* txq);

/**
 * @brief Release queue resources.
 * @param txq Queue instance.
 */
void stt_audio_txq_cleanup(stt_audio_txq_t* txq);

/**
 * @brief Mark the worker as started so push operations are accepted.
 * @param txq Queue instance.
 */
void stt_audio_txq_worker_started(stt_audio_txq_t* txq);

/**
 * @brief Push one chunk, shedding the oldest if full. Never blocks the caller.
 * @param txq Queue instance.
 * @param pcm Audio samples (S16_LE mono).
 * @param size Byte count.
 * @param timestamp_ns Capture timestamp.
 * @param dropped Chunks dropped by the capture queue so far.
 * @return 0 on success, or -EINVAL on bad arguments, or -EAGAIN if worker not started.
 */
int stt_audio_txq_push(stt_audio_txq_t* txq,
                       void const* pcm,
                       size_t size,
                       uint64_t timestamp_ns,
                       uint32_t dropped);

/**
 * @brief Pop the oldest chunk for transmission. Returns -EAGAIN when empty.
 * @param txq Queue instance.
 * @param chunk Destination; dropped field includes internal drops.
 * @return 0 on success, or -EAGAIN when empty.
 */
int stt_audio_txq_pop(stt_audio_txq_t* txq, stt_audio_txq_chunk_t* chunk);

/**
 * @brief Discard all pending chunks and count them as dropped.
 * @param txq Queue instance.
 */
void stt_audio_txq_discard_all(stt_audio_txq_t* txq);

/**
 * @brief Request worker stop and wake it.
 * @param txq Queue instance.
 */
void stt_audio_txq_request_stop(stt_audio_txq_t* txq);

/**
 * @brief Return nonzero if stop was requested.
 * @param txq Queue instance.
 * @return 1 if stop requested, 0 otherwise.
 */
uint8_t stt_audio_txq_stop_requested(stt_audio_txq_t* txq);

/**
 * @brief Wait for audio or stop signal, bounded so timers can progress.
 * @param txq Queue instance.
 * @param timeout_ms Maximum wait time in milliseconds.
 */
void stt_audio_txq_wait(stt_audio_txq_t* txq, uint32_t timeout_ms);

/**
 * @brief Get the count of chunks dropped due to queue overflow.
 * @param txq Queue instance.
 * @return Cumulative drop count.
 */
uint32_t stt_audio_txq_get_dropped_count(stt_audio_txq_t* txq);

/**
 * @brief Get the number of chunks currently waiting for transmission.
 * @param txq Queue instance.
 * @return Pending chunk count, or 0 when @p txq is NULL.
 */
uint32_t stt_audio_txq_get_count(stt_audio_txq_t* txq);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
