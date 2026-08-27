/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file test_stt_event_ring.c
/// @brief Unit tests for the STT transcript event ring
///

// === Headers files inclusions ==================================================================================== //

#include "stt_event_ring.h"
#include "stt_json.h"
#include "stt_transcript_parse.h"
#include "number_parse.h"
#include "unity.h"

#include <errno.h>
#include <string.h>

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //

static stt_event_ring_t ring;

// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

void setUp(void)
{
    memset(&ring, 0, sizeof(ring));
}

void tearDown(void)
{
    stt_event_ring_cleanup(&ring);
}

void test_stt_event_ring_init_succeeds(void)
{
    int const ret = stt_event_ring_init(&ring);

    TEST_ASSERT_EQUAL_INT(0, ret);
    stt_event_ring_cleanup(&ring);
}

void test_stt_event_ring_init_rejects_null(void)
{
    int const ret = stt_event_ring_init(NULL);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ret);
}

void test_stt_event_ring_pushes_and_drains_one_event(void)
{
    char const* const line =
        "{\"seq\":42,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"hola\"}";
    subtitle_text_evt_t events[8];
    uint32_t event_count = 0U;
    int ret;

    stt_event_ring_init(&ring);

    ret = stt_event_ring_push(&ring, line, strlen(line));
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = stt_event_ring_drain(&ring, events, 8U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1U, event_count);
    TEST_ASSERT_EQUAL_UINT32(42U, events[0].seq);
    TEST_ASSERT_EQUAL_STRING("hola", events[0].text);
}

void test_stt_event_ring_sheds_oldest_partial_when_full(void)
{
    subtitle_text_evt_t events[STT_EVENT_RING_DEPTH + 1U];
    uint32_t event_count = 0U;
    unsigned int i;
    int ret;

    stt_event_ring_init(&ring);

    for (i = 0U; i < STT_EVENT_RING_DEPTH; i++)
    {
        char line[128];

        snprintf(line, sizeof(line),
                 "{\"seq\":%u,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"partial%u\"}",
                 i, i);
        ret = stt_event_ring_push(&ring, line, strlen(line));
        TEST_ASSERT_EQUAL_INT(0, ret);
    }

    char const* const new_line =
        "{\"seq\":99,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"new\"}";
    ret = stt_event_ring_push(&ring, new_line, strlen(new_line));
    TEST_ASSERT_EQUAL_INT(0, ret);

    TEST_ASSERT_EQUAL_UINT32(1U, stt_event_ring_get_dropped_count(&ring));

    ret = stt_event_ring_drain(&ring, events, STT_EVENT_RING_DEPTH + 1U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(STT_EVENT_RING_DEPTH, event_count);
    TEST_ASSERT_EQUAL_UINT32(1U, events[0].seq);
    TEST_ASSERT_EQUAL_STRING("partial1", events[0].text);
}

void test_stt_event_ring_sheds_oldest_final_when_only_finals_present(void)
{
    subtitle_text_evt_t events[STT_EVENT_RING_DEPTH + 1U];
    uint32_t event_count = 0U;
    unsigned int i;
    int ret;

    stt_event_ring_init(&ring);

    for (i = 0U; i < STT_EVENT_RING_DEPTH; i++)
    {
        char line[128];

        snprintf(line, sizeof(line),
                 "{\"seq\":%u,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"final%u\"}",
                 i, i);
        ret = stt_event_ring_push(&ring, line, strlen(line));
        TEST_ASSERT_EQUAL_INT(0, ret);
    }

    char const* const new_line =
        "{\"seq\":99,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"new\"}";
    ret = stt_event_ring_push(&ring, new_line, strlen(new_line));
    TEST_ASSERT_EQUAL_INT(0, ret);

    TEST_ASSERT_EQUAL_UINT32(1U, stt_event_ring_get_dropped_count(&ring));

    ret = stt_event_ring_drain(&ring, events, STT_EVENT_RING_DEPTH + 1U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(STT_EVENT_RING_DEPTH, event_count);
    TEST_ASSERT_EQUAL_UINT32(1U, events[0].seq);
    TEST_ASSERT_EQUAL_STRING("final1", events[0].text);
}

void test_stt_event_ring_sheds_oldest_partial_before_finals(void)
{
    subtitle_text_evt_t events[STT_EVENT_RING_DEPTH + 1U];
    uint32_t event_count = 0U;
    int ret;

    stt_event_ring_init(&ring);

    for (unsigned int i = 0U; i < STT_EVENT_RING_DEPTH; i++)
    {
        char line[128];
        uint8_t const is_final = (i % 2U) == 0U;

        snprintf(line, sizeof(line),
                 "{\"seq\":%u,\"is_final\":%s,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"%s%u\"}",
                 i, is_final ? "true" : "false", is_final ? "final" : "partial", i);
        ret = stt_event_ring_push(&ring, line, strlen(line));
        TEST_ASSERT_EQUAL_INT(0, ret);
    }

    char const* const new_line =
        "{\"seq\":99,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"new\"}";
    ret = stt_event_ring_push(&ring, new_line, strlen(new_line));
    TEST_ASSERT_EQUAL_INT(0, ret);

    TEST_ASSERT_EQUAL_UINT32(1U, stt_event_ring_get_dropped_count(&ring));

    ret = stt_event_ring_drain(&ring, events, STT_EVENT_RING_DEPTH + 1U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(STT_EVENT_RING_DEPTH, event_count);
    TEST_ASSERT_EQUAL_UINT32(0U, events[0].seq);
    TEST_ASSERT_EQUAL_STRING("final0", events[0].text);
}

void test_stt_event_ring_generation_bump_invalidates_copied_entries(void)
{
    subtitle_text_evt_t events[8];
    uint32_t event_count = 0U;
    uint32_t discarded;
    int ret;

    stt_event_ring_init(&ring);

    char const* const line1 =
        "{\"seq\":1,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"old\"}";
    ret = stt_event_ring_push(&ring, line1, strlen(line1));
    TEST_ASSERT_EQUAL_INT(0, ret);

    discarded = stt_event_ring_invalidate_session(&ring);
    TEST_ASSERT_EQUAL_UINT32(1U, discarded);

    char const* const line2 =
        "{\"seq\":0,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"new\"}";
    ret = stt_event_ring_push(&ring, line2, strlen(line2));
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = stt_event_ring_drain(&ring, events, 8U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1U, event_count);
    TEST_ASSERT_EQUAL_UINT32(0U, events[0].seq);
    TEST_ASSERT_EQUAL_STRING("new", events[0].text);
}

void test_stt_event_ring_rejects_duplicate_sequence(void)
{
    subtitle_text_evt_t events[8];
    uint32_t event_count = 0U;
    int ret;

    stt_event_ring_init(&ring);

    char const* const line1 =
        "{\"seq\":5,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"first\"}";
    ret = stt_event_ring_push(&ring, line1, strlen(line1));
    TEST_ASSERT_EQUAL_INT(0, ret);

    char const* const line2 =
        "{\"seq\":5,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"duplicate\"}";
    ret = stt_event_ring_push(&ring, line2, strlen(line2));
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = stt_event_ring_drain(&ring, events, 8U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1U, event_count);
    TEST_ASSERT_EQUAL_STRING("first", events[0].text);
    TEST_ASSERT_EQUAL_UINT32(1U, stt_event_ring_get_rejected_count(&ring));
}

void test_stt_event_ring_rejects_out_of_order_sequence(void)
{
    subtitle_text_evt_t events[8];
    uint32_t event_count = 0U;
    int ret;

    stt_event_ring_init(&ring);

    char const* const line1 =
        "{\"seq\":10,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"first\"}";
    ret = stt_event_ring_push(&ring, line1, strlen(line1));
    TEST_ASSERT_EQUAL_INT(0, ret);

    char const* const line2 =
        "{\"seq\":5,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"old\"}";
    ret = stt_event_ring_push(&ring, line2, strlen(line2));
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = stt_event_ring_drain(&ring, events, 8U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1U, event_count);
    TEST_ASSERT_EQUAL_STRING("first", events[0].text);
    TEST_ASSERT_EQUAL_UINT32(1U, stt_event_ring_get_rejected_count(&ring));
}

void test_stt_event_ring_accepts_seq_zero_after_generation_bump(void)
{
    subtitle_text_evt_t events[8];
    uint32_t event_count = 0U;
    int ret;

    stt_event_ring_init(&ring);

    char const* const line1 =
        "{\"seq\":42,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"old\"}";
    ret = stt_event_ring_push(&ring, line1, strlen(line1));
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = stt_event_ring_drain(&ring, events, 8U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1U, event_count);

    stt_event_ring_invalidate_session(&ring);

    char const* const line2 =
        "{\"seq\":0,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"new\"}";
    ret = stt_event_ring_push(&ring, line2, strlen(line2));
    TEST_ASSERT_EQUAL_INT(0, ret);

    event_count = 0U;
    ret = stt_event_ring_drain(&ring, events, 8U, &event_count);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1U, event_count);
    TEST_ASSERT_EQUAL_UINT32(0U, events[0].seq);
}

void test_stt_event_ring_rejects_line_too_long(void)
{
    char long_line[STT_EVENT_RING_LINE_MAX + 10U];
    int ret;

    stt_event_ring_init(&ring);

    memset(long_line, 'A', sizeof(long_line));
    long_line[sizeof(long_line) - 1U] = '\0';

    ret = stt_event_ring_push(&ring, long_line, sizeof(long_line) - 1U);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ret);
}
