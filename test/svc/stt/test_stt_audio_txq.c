/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file test_stt_audio_txq.c
/// @brief Unit tests for the STT audio transmit queue
///

// === Headers files inclusions ==================================================================================== //

#include "stt_audio_txq.h"
#include "unity.h"

#include <errno.h>
#include <string.h>

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //

static stt_audio_txq_t txq;

// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

void setUp(void)
{
    memset(&txq, 0, sizeof(txq));
}

void tearDown(void)
{
    stt_audio_txq_cleanup(&txq);
}

void test_stt_audio_txq_init_succeeds(void)
{
    int const ret = stt_audio_txq_init(&txq);

    TEST_ASSERT_EQUAL_INT(0, ret);
    stt_audio_txq_cleanup(&txq);
}

void test_stt_audio_txq_init_rejects_null(void)
{
    int const ret = stt_audio_txq_init(NULL);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ret);
}

void test_stt_audio_txq_push_rejects_invalid_arguments(void)
{
    uint8_t data[128];
    int ret;

    stt_audio_txq_init(&txq);
    stt_audio_txq_worker_started(&txq);

    ret = stt_audio_txq_push(NULL, data, sizeof(data), 1000ULL, 0U);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ret);

    ret = stt_audio_txq_push(&txq, NULL, sizeof(data), 1000ULL, 0U);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ret);

    ret = stt_audio_txq_push(&txq, data, 0U, 1000ULL, 0U);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ret);

    ret = stt_audio_txq_push(&txq, data, STT_AUDIO_TXQ_MAX_BYTES + 1U, 1000ULL, 0U);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ret);
}

void test_stt_audio_txq_rejects_push_before_worker_started(void)
{
    uint8_t data[128];
    int const ret = stt_audio_txq_init(&txq);

    TEST_ASSERT_EQUAL_INT(0, ret);

    int const push_ret = stt_audio_txq_push(&txq, data, sizeof(data), 1000ULL, 0U);

    TEST_ASSERT_EQUAL_INT(-EAGAIN, push_ret);
}

void test_stt_audio_txq_pushes_and_pops_one_chunk(void)
{
    uint8_t data[128];
    stt_audio_txq_chunk_t chunk;
    int ret;

    memset(data, 0xAB, sizeof(data));
    stt_audio_txq_init(&txq);
    stt_audio_txq_worker_started(&txq);

    ret = stt_audio_txq_push(&txq, data, sizeof(data), 1234ULL, 5U);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = stt_audio_txq_pop(&txq, &chunk);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_size_t(sizeof(data), chunk.size);
    TEST_ASSERT_EQUAL_UINT64(1234ULL, chunk.timestamp_ns);
    TEST_ASSERT_EQUAL_UINT32(5U, chunk.dropped);
    TEST_ASSERT_EQUAL_UINT8(0xAB, chunk.payload[0]);
}

void test_stt_audio_txq_pop_returns_eagain_when_empty(void)
{
    stt_audio_txq_chunk_t chunk;
    int const ret = stt_audio_txq_init(&txq);

    TEST_ASSERT_EQUAL_INT(0, ret);

    int const pop_ret = stt_audio_txq_pop(&txq, &chunk);

    TEST_ASSERT_EQUAL_INT(-EAGAIN, pop_ret);
}

void test_stt_audio_txq_drops_oldest_when_full(void)
{
    uint8_t data[128];
    stt_audio_txq_chunk_t chunk;
    int ret;
    unsigned int i;

    stt_audio_txq_init(&txq);
    stt_audio_txq_worker_started(&txq);

    for (i = 0U; i < STT_AUDIO_TXQ_DEPTH; i++)
    {
        data[0] = (uint8_t)i;
        ret = stt_audio_txq_push(&txq, data, sizeof(data), (uint64_t)i * 1000ULL, 0U);
        TEST_ASSERT_EQUAL_INT(0, ret);
    }

    data[0] = 0xFFU;
    ret = stt_audio_txq_push(&txq, data, sizeof(data), 999000ULL, 0U);
    TEST_ASSERT_EQUAL_INT(0, ret);

    TEST_ASSERT_EQUAL_UINT32(1U, stt_audio_txq_get_dropped_count(&txq));

    ret = stt_audio_txq_pop(&txq, &chunk);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT8(1U, chunk.payload[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, chunk.dropped);
}

void test_stt_audio_txq_discard_all_clears_queue_and_counts_drops(void)
{
    uint8_t data[128];
    stt_audio_txq_chunk_t chunk;
    int ret;
    unsigned int i;

    stt_audio_txq_init(&txq);
    stt_audio_txq_worker_started(&txq);

    for (i = 0U; i < 5U; i++)
    {
        data[0] = (uint8_t)i;
        ret = stt_audio_txq_push(&txq, data, sizeof(data), (uint64_t)i * 1000ULL, 0U);
        TEST_ASSERT_EQUAL_INT(0, ret);
    }

    stt_audio_txq_discard_all(&txq);

    ret = stt_audio_txq_pop(&txq, &chunk);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, ret);

    TEST_ASSERT_EQUAL_UINT32(5U, stt_audio_txq_get_dropped_count(&txq));
}

void test_stt_audio_txq_stop_request_prevents_new_pushes(void)
{
    uint8_t data[128];
    int ret;

    stt_audio_txq_init(&txq);
    stt_audio_txq_worker_started(&txq);

    ret = stt_audio_txq_push(&txq, data, sizeof(data), 1000ULL, 0U);
    TEST_ASSERT_EQUAL_INT(0, ret);

    stt_audio_txq_request_stop(&txq);

    ret = stt_audio_txq_push(&txq, data, sizeof(data), 2000ULL, 0U);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, ret);

    TEST_ASSERT_EQUAL_UINT8(1U, stt_audio_txq_stop_requested(&txq));
}

void test_stt_audio_txq_preserves_fifo_order(void)
{
    uint8_t data[128];
    stt_audio_txq_chunk_t chunk;
    int ret;
    unsigned int i;

    stt_audio_txq_init(&txq);
    stt_audio_txq_worker_started(&txq);

    for (i = 0U; i < 8U; i++)
    {
        data[0] = (uint8_t)i;
        ret = stt_audio_txq_push(&txq, data, sizeof(data), (uint64_t)i * 1000ULL, 0U);
        TEST_ASSERT_EQUAL_INT(0, ret);
    }

    for (i = 0U; i < 8U; i++)
    {
        ret = stt_audio_txq_pop(&txq, &chunk);
        TEST_ASSERT_EQUAL_INT(0, ret);
        TEST_ASSERT_EQUAL_UINT8(i, chunk.payload[0]);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i * 1000ULL, chunk.timestamp_ns);
    }
}
