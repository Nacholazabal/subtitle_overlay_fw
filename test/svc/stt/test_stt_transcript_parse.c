#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "app.h"
#include "stt_transcript_parse.h"

TEST_SOURCE_FILE("number_parse.c")
TEST_SOURCE_FILE("stt_json.c")

static subtitle_text_evt_t event;

void setUp(void)
{
    memset(&event, 0, sizeof(event));
}

void tearDown(void)
{}

void test_stt_transcript_parse_line_rejects_empty_text(void)
{
    char const* const line =
        "{\"seq\":11,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":0.1,\"text\":\"\"}";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(line, &event));
}

void test_stt_transcript_parse_line_rejects_end_before_start(void)
{
    char const* const line =
        "{\"seq\":12,\"is_final\":false,\"start_sec\":2.0,\"end_sec\":1.0,\"text\":\"x\"}";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(line, &event));
}

void test_stt_transcript_parse_line_rejects_missing_required_fields(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_transcript_parse_line("{\"is_final\":false,\"start_sec\":0.0,"
                                                  "\"end_sec\":1.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"end_sec\":1.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0.0,\"end_sec\":1.0}",
                                                  &event));
}

void test_stt_transcript_parse_line_handles_simple_escapes(void)
{
    char const* const line = "{\"seq\":13,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,"
                             "\"text\":\"hola\\n\\\"mundo\\\"\\\\\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(line, &event));
    TEST_ASSERT_EQUAL_STRING("hola \"mundo\"\\", event.text);
}

void test_stt_transcript_parse_line_accepts_sender_fields_in_any_order(void)
{
    char const* const line = "{\"text\":\"hola\",\"dropped\":0,\"end_sec\":1.25,"
                             "\"type\":\"final\",\"seq\":21,\"chunk_start\":1,"
                             "\"start_sec\":1.0,\"is_final\":true}";

    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(21U, event.seq);
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_STRING("hola", event.text);
}

void test_stt_transcript_parse_line_does_not_match_keys_inside_text(void)
{
    char const* const line = "{\"text\":\"say \\\"seq\\\":999 now\",\"seq\":22,"
                             "\"is_final\":false,\"start_sec\":0.0,\"end_sec\":0.2}";

    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(22U, event.seq);
    TEST_ASSERT_EQUAL_STRING("say \"seq\":999 now", event.text);
}

void test_stt_transcript_parse_line_rejects_duplicates_and_inconsistent_final_fields(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_transcript_parse_line("{\"seq\":1,\"seq\":2,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,\"type\":\"final\","
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                &event));
}

void test_stt_transcript_parse_line_rejects_overflow_and_malformed_numeric_tokens(void)
{
    TEST_ASSERT_EQUAL_INT(-ERANGE,
                          stt_transcript_parse_line("{\"seq\":4294967296,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_transcript_parse_line("{\"seq\":1junk,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":4294968,\"text\":\"x\"}",
                                &event));
}

void test_stt_transcript_parse_line_rejects_malformed_escapes_and_trailing_garbage(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"bad\\q\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}junk",
                                &event));
}

void test_stt_transcript_parse_line_rejects_nonfinite_time_and_trailing_comma(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1e999,\"text\":\"x\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_transcript_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\",}",
                                                  &event));
}

// The streaming server emits this shape verbatim (server/runtime/nemotron.py
// TranscriptAdapter._emit). Only the PC bridge used to trim it down to five
// keys; the board must consume the full event once it talks to the server
// directly, arrays and all.
void test_stt_transcript_parse_line_accepts_full_streaming_server_event(void)
{
    char const* const line =
        "{\"type\":\"transcript\",\"seq\":7,\"is_final\":true,\"start_sec\":12.345,"
        "\"end_sec\":15.678,\"text\":\"hola mundo\",\"full_text\":\"hola mundo\","
        "\"run_engine\":\"nemotron_3_5_nemo\","
        "\"nemo_commit\":\"2639d4bef8d1450782263a8f616242acfb6fecb9\","
        "\"target_lang\":\"es-ES\",\"lookahead_ms\":560,\"att_context_size\":[56,6],"
        "\"timestamp_source\":\"nemo_segments\",\"emit_monotonic\":123456.789012,"
        "\"eou\":true,\"final_reason\":\"model_eou\",\"gpu_infer_sec\":0.0123,"
        "\"server_sent_monotonic\":123456.789012}";

    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(7U, event.seq);
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_UINT32(12345U, event.start_ms);
    TEST_ASSERT_EQUAL_UINT32(15678U, event.end_ms);
    TEST_ASSERT_EQUAL_STRING("hola mundo", event.text);
}

void test_stt_transcript_parse_line_treats_transcript_type_as_non_finality_marker(void)
{
    // "transcript" is the WebSocket message discriminator, so is_final decides.
    char const* const with_final = "{\"type\":\"transcript\",\"seq\":8,\"is_final\":false,"
                                   "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}";
    // Without is_final there is no finality information at all: reject.
    char const* const without_final = "{\"type\":\"transcript\",\"seq\":9,"
                                      "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}";
    // An unrelated type value stays invalid.
    char const* const unknown_type = "{\"type\":\"pong\",\"seq\":10,\"is_final\":true,"
                                     "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(with_final, &event));
    TEST_ASSERT_EQUAL_UINT8(0U, event.is_final);

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(without_final, &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(unknown_type, &event));
}

void test_stt_transcript_parse_line_skips_unknown_arrays_and_nested_objects(void)
{
    char const* const empty_containers =
        "{\"seq\":1,\"is_final\":false,\"start_sec\":0,\"end_sec\":1,\"text\":\"x\","
        "\"empty_array\":[],\"empty_object\":{}}";
    char const* const nested =
        "{\"seq\":2,\"is_final\":false,\"start_sec\":0,\"end_sec\":1,\"text\":\"y\","
        "\"nested\":{\"a\":[1,2,{\"b\":\"c\"}],\"d\":null}}";
    char const* const strings_with_braces =
        "{\"seq\":3,\"is_final\":false,\"start_sec\":0,\"end_sec\":1,\"text\":\"z\","
        "\"tricky\":[\"],\\\"seq\\\":999\",\"}\"]}";

    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(empty_containers, &event));
    TEST_ASSERT_EQUAL_UINT32(1U, event.seq);

    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(nested, &event));
    TEST_ASSERT_EQUAL_UINT32(2U, event.seq);

    // A ']' or '}' inside a string must not be mistaken for a container close.
    TEST_ASSERT_EQUAL_INT(0, stt_transcript_parse_line(strings_with_braces, &event));
    TEST_ASSERT_EQUAL_UINT32(3U, event.seq);
    TEST_ASSERT_EQUAL_STRING("z", event.text);
}

void test_stt_transcript_parse_line_rejects_malformed_and_too_deep_containers(void)
{
    char const* const unterminated =
        "{\"seq\":1,\"is_final\":false,\"start_sec\":0,\"end_sec\":1,\"text\":\"x\","
        "\"bad\":[1,2}";
    char const* const missing_colon =
        "{\"seq\":1,\"is_final\":false,\"start_sec\":0,\"end_sec\":1,\"text\":\"x\","
        "\"bad\":{\"a\" 1}}";
    char const* const trailing_comma =
        "{\"seq\":1,\"is_final\":false,\"start_sec\":0,\"end_sec\":1,\"text\":\"x\","
        "\"bad\":[1,]}";
    // STT_EVENT_RX_JSON_MAX_DEPTH is 4: five nested arrays must be refused
    // instead of recursing deeper.
    char const* const too_deep =
        "{\"seq\":1,\"is_final\":false,\"start_sec\":0,\"end_sec\":1,\"text\":\"x\","
        "\"bad\":[[[[[1]]]]]}";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(unterminated, &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(missing_colon, &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(trailing_comma, &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_transcript_parse_line(too_deep, &event));
}
