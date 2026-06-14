#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "app.h"
#include "errorno.h"
#include "stt_event_rx.h"

static subtitle_text_evt_t event;

void setUp(void)
{
    memset(&event, 0, sizeof(event));
}

void tearDown(void)
{
}

void test_stt_event_rx_parse_line_accepts_valid_partial_with_boolean_final_flag(void)
{
    char const* const line =
        "{\"seq\":7,\"is_final\":false,\"start_sec\":1.25,\"end_sec\":1.75,\"text\":\"hola\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(7U, event.seq);
    TEST_ASSERT_EQUAL_UINT8(0U, event.is_final);
    TEST_ASSERT_EQUAL_UINT32(1250U, event.start_ms);
    TEST_ASSERT_EQUAL_UINT32(1750U, event.end_ms);
    TEST_ASSERT_EQUAL_STRING("hola", event.text);
}

void test_stt_event_rx_parse_line_accepts_valid_final_with_type_field(void)
{
    char const* const line =
        "{\"seq\":8,\"type\":\"final\",\"start_sec\":2.0,\"end_sec\":2.5,\"text\":\"listo\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_STRING("listo", event.text);
}

void test_stt_event_rx_parse_line_accepts_partial_type_field(void)
{
    char const* const line =
        "{\"seq\":9,\"type\":\"partial\",\"start_sec\":0.0,\"end_sec\":0.1,\"text\":\"va\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT8(0U, event.is_final);
}

void test_stt_event_rx_parse_line_truncates_long_text_but_keeps_event(void)
{
    char line[512];
    char long_text[256];

    memset(long_text, 'a', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';
    snprintf(line,
             sizeof(line),
             "{\"seq\":10,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"%s\"}",
             long_text);

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_UINT(SUBTITLE_TEXT_MAX_LEN - 1U, strlen(event.text));
    TEST_ASSERT_EQUAL_CHAR('a', event.text[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', event.text[SUBTITLE_TEXT_MAX_LEN - 1U]);
}

void test_stt_event_rx_parse_line_rejects_empty_text(void)
{
    char const* const line =
        "{\"seq\":11,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":0.1,\"text\":\"\"}";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_event_rx_parse_line(line, &event));
}

void test_stt_event_rx_parse_line_rejects_end_before_start(void)
{
    char const* const line =
        "{\"seq\":12,\"is_final\":false,\"start_sec\":2.0,\"end_sec\":1.0,\"text\":\"x\"}";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_event_rx_parse_line(line, &event));
}

void test_stt_event_rx_parse_line_rejects_missing_required_fields(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"is_final\":false,\"start_sec\":0.0,"
                                                  "\"end_sec\":1.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"end_sec\":1.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0.0,\"end_sec\":1.0}",
                                                  &event));
}

void test_stt_event_rx_parse_line_handles_simple_escapes(void)
{
    char const* const line =
        "{\"seq\":13,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,"
        "\"text\":\"hola\\n\\\"mundo\\\"\\\\\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_STRING("hola \"mundo\"\\", event.text);
}
