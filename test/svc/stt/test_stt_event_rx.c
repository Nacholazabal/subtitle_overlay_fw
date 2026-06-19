#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "app.h"
#include "errorno.h"
#include "stt_event_rx.h"

TEST_SOURCE_FILE("number_parse.c")

static subtitle_text_evt_t event;

void setUp(void)
{
    memset(&event, 0, sizeof(event));
    unsetenv("SUBTITLE_STT_RX_HOST");
    unsetenv("SUBTITLE_STT_RX_PORT");
}

void tearDown(void)
{
    unsetenv("SUBTITLE_STT_RX_HOST");
    unsetenv("SUBTITLE_STT_RX_PORT");
}

void test_stt_event_rx_default_config_accepts_valid_and_ignores_invalid_port_overrides(void)
{
    stt_event_rx_config_t config;

    stt_event_rx_default_config(NULL);
    setenv("SUBTITLE_STT_RX_HOST", "127.0.0.1", 1);
    setenv("SUBTITLE_STT_RX_PORT", "6001", 1);
    stt_event_rx_default_config(&config);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", config.host);
    TEST_ASSERT_EQUAL_UINT32(6001U, config.port);

    setenv("SUBTITLE_STT_RX_PORT", "70000", 1);
    stt_event_rx_default_config(&config);
    TEST_ASSERT_EQUAL_UINT32(STT_EVENT_RX_DEFAULT_PORT, config.port);

    setenv("SUBTITLE_STT_RX_PORT", "5001junk", 1);
    stt_event_rx_default_config(&config);
    TEST_ASSERT_EQUAL_UINT32(STT_EVENT_RX_DEFAULT_PORT, config.port);
}

void test_stt_event_rx_init_rejects_port_outside_tcp_range(void)
{
    stt_event_rx_t rx;
    stt_event_rx_config_t config;

    memset(&rx, 0, sizeof(rx));
    memset(&config, 0, sizeof(config));
    snprintf(config.host, sizeof(config.host), "%s", "127.0.0.1");
    config.port = 65536U;

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_event_rx_init(&rx, &config));
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
    char const* const line = "{\"seq\":13,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,"
                             "\"text\":\"hola\\n\\\"mundo\\\"\\\\\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_STRING("hola \"mundo\"\\", event.text);
}

void test_stt_event_rx_parse_line_accepts_sender_fields_in_any_order(void)
{
    char const* const line = "{\"text\":\"hola\",\"dropped\":0,\"end_sec\":1.25,"
                             "\"type\":\"final\",\"seq\":21,\"chunk_start\":1,"
                             "\"start_sec\":1.0,\"is_final\":true}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(21U, event.seq);
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_STRING("hola", event.text);
}

void test_stt_event_rx_parse_line_does_not_match_keys_inside_text(void)
{
    char const* const line = "{\"text\":\"say \\\"seq\\\":999 now\",\"seq\":22,"
                             "\"is_final\":false,\"start_sec\":0.0,\"end_sec\":0.2}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(22U, event.seq);
    TEST_ASSERT_EQUAL_STRING("say \"seq\":999 now", event.text);
}

void test_stt_event_rx_parse_line_rejects_duplicates_and_inconsistent_final_fields(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"seq\":2,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,\"type\":\"final\","
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                &event));
}

void test_stt_event_rx_parse_line_rejects_overflow_and_malformed_numeric_tokens(void)
{
    TEST_ASSERT_EQUAL_INT(
        -ERANGE,
        stt_event_rx_parse_line("{\"seq\":4294967296,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1junk,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":4294968,\"text\":\"x\"}",
                                &event));
}

void test_stt_event_rx_parse_line_rejects_malformed_escapes_and_trailing_garbage(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"bad\\q\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}junk",
                                &event));
}

void test_stt_event_rx_parse_line_rejects_nonfinite_time_and_trailing_comma(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1e999,\"text\":\"x\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\",}",
                                &event));
}
