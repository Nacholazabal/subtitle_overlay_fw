#include <errno.h>
#include <string.h>

#include "unity.h"
#include "stt_session_json.h"

TEST_SOURCE_FILE("number_parse.c")
TEST_SOURCE_FILE("stt_json.c")

static char out[1024];
static stt_session_ready_t ready;
static stt_session_error_t error;

/// @brief The board's real capture format: 48 kHz mono S16_LE in 20 ms chunks.
static stt_session_start_t board_start(void)
{
    stt_session_start_t start;

    memset(&start, 0, sizeof(start));
    start.sample_rate_hz = 48000U;
    start.channels = 1U;
    start.format = STT_SESSION_FORMAT_S16_LE;
    start.chunk_ms = 20U;
    start.samples_per_chunk = 960U;
    start.bytes_per_chunk = 1920U;
    return start;
}

void setUp(void)
{
    memset(out, 0, sizeof(out));
    memset(&ready, 0, sizeof(ready));
    memset(&error, 0, sizeof(error));
}

void tearDown(void)
{}

// === session_start =============================================================================================== //

void test_stt_session_start_matches_what_the_server_validates(void)
{
    stt_session_start_t const start = board_start();
    int const length = stt_session_json_build_start(out, sizeof(out), &start);

    // validate_session_start() in server/runtime/protocol.py checks exactly these.
    TEST_ASSERT_GREATER_THAN_INT(0, length);
    TEST_ASSERT_EQUAL_INT((int)strlen(out), length);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"session_start\",\"version\":1,"
                             "\"sample_rate_hz\":48000,\"channels\":1,\"format\":1,"
                             "\"chunk_ms\":20,\"samples_per_chunk\":960,"
                             "\"bytes_per_chunk\":1920}",
                             out);
}

void test_stt_session_start_carries_the_chosen_nemotron_operating_point(void)
{
    stt_session_start_t start = board_start();
    int length;

    // The configuration selected by the thesis: 560 ms lookahead, 600 ms EOU.
    start.latency_ms = 560U;
    start.stop_history_eou_ms = 600U;
    start.residue_tokens_at_end = 2U;
    snprintf(start.target_lang, sizeof(start.target_lang), "%s", "es-ES");

    length = stt_session_json_build_start(out, sizeof(out), &start);

    TEST_ASSERT_GREATER_THAN_INT(0, length);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"backend_config\":{\"target_lang\":\"es-ES\","
                                     "\"latency_ms\":560,\"stop_history_eou_ms\":600,"
                                     "\"residue_tokens_at_end\":2}"));
}

void test_stt_session_start_omits_unset_backend_overrides(void)
{
    stt_session_start_t start = board_start();

    // Only a numeric override: the object must not start with a stray comma.
    start.latency_ms = 320U;
    TEST_ASSERT_GREATER_THAN_INT(0, stt_session_json_build_start(out, sizeof(out), &start));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"backend_config\":{\"latency_ms\":320}"));
    TEST_ASSERT_NULL(strstr(out, "{,"));
    TEST_ASSERT_NULL(strstr(out, "target_lang"));

    // Only a language: no trailing comma either.
    start = board_start();
    snprintf(start.target_lang, sizeof(start.target_lang), "%s", "es-ES");
    TEST_ASSERT_GREATER_THAN_INT(0, stt_session_json_build_start(out, sizeof(out), &start));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"backend_config\":{\"target_lang\":\"es-ES\"}"));
}

void test_stt_session_start_rejects_formats_the_server_would_refuse(void)
{
    stt_session_start_t start = board_start();

    start.channels = 2U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_build_start(out, sizeof(out), &start));

    start = board_start();
    start.format = 2U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_build_start(out, sizeof(out), &start));

    start = board_start();
    start.sample_rate_hz = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_build_start(out, sizeof(out), &start));

    start = board_start();
    start.bytes_per_chunk = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_build_start(out, sizeof(out), &start));
}

void test_stt_session_start_refuses_a_language_tag_that_could_inject_json(void)
{
    stt_session_start_t start = board_start();

    // The tag comes from the environment; it must never close the string.
    snprintf(start.target_lang, sizeof(start.target_lang), "%s", "es\",\"x\":\"y");
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_build_start(out, sizeof(out), &start));
}

void test_stt_session_start_reports_a_buffer_that_is_too_small(void)
{
    stt_session_start_t const start = board_start();

    TEST_ASSERT_EQUAL_INT(-ENOBUFS, stt_session_json_build_start(out, 32U, &start));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_build_start(NULL, sizeof(out), &start));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_build_start(out, sizeof(out), NULL));
}

// === Message dispatch ============================================================================================ //

void test_stt_session_message_type_identifies_every_server_message(void)
{
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_SESSION_READY,
                          stt_session_json_message_type("{\"type\":\"session_ready\"}"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_TRANSCRIPT,
                          stt_session_json_message_type("{\"type\":\"transcript\",\"seq\":0}"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_SESSION_SUMMARY,
                          stt_session_json_message_type("{\"type\":\"session_summary\"}"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_ERROR,
                          stt_session_json_message_type("{\"type\":\"error\"}"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_PONG, stt_session_json_message_type("{\"type\":\"pong\"}"));
}

void test_stt_session_message_type_is_unknown_for_junk_and_missing_type(void)
{
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_UNKNOWN, stt_session_json_message_type("{\"seq\":1}"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_UNKNOWN,
                          stt_session_json_message_type("{\"type\":\"whatever\"}"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_UNKNOWN, stt_session_json_message_type("not json"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_UNKNOWN, stt_session_json_message_type("{\"type\":1}"));
    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_UNKNOWN, stt_session_json_message_type("{}"));
}

void test_stt_session_message_type_skips_nested_values_before_the_type_key(void)
{
    // `type` is not always first, and earlier keys may hold arrays or objects.
    char const* const json = "{\"att_context_size\":[56,6],\"run_config\":{\"a\":{\"b\":1}},"
                             "\"type\":\"transcript\"}";

    TEST_ASSERT_EQUAL_INT(STT_SESSION_MSG_TRANSCRIPT, stt_session_json_message_type(json));
}

// === session_ready =============================================================================================== //

void test_stt_session_ready_captures_the_effective_run_config_verbatim(void)
{
    char const* const json =
        "{\"type\":\"session_ready\",\"version\":1,\"sample_rate_hz\":48000,"
        "\"run_engine\":\"nemotron_3_5_nemo\","
        "\"run_config\":{\"config_latency_ms\":560,\"config_target_lang\":\"es-ES\","
        "\"config_att_context_size\":[56,6]}}";

    TEST_ASSERT_EQUAL_INT(0, stt_session_json_parse_ready(json, &ready));
    TEST_ASSERT_EQUAL_UINT32(1U, ready.version);
    TEST_ASSERT_EQUAL_UINT32(48000U, ready.sample_rate_hz);
    TEST_ASSERT_EQUAL_STRING("nemotron_3_5_nemo", ready.run_engine);
    TEST_ASSERT_EQUAL_UINT8(0U, ready.run_config_truncated);
    // Kept as raw JSON so the log records exactly what the server negotiated.
    TEST_ASSERT_EQUAL_STRING("{\"config_latency_ms\":560,\"config_target_lang\":\"es-ES\","
                             "\"config_att_context_size\":[56,6]}",
                             ready.run_config);
}

void test_stt_session_ready_rejects_an_unsupported_protocol_version(void)
{
    char const* const newer = "{\"type\":\"session_ready\",\"version\":2,\"run_engine\":\"x\"}";
    char const* const missing = "{\"type\":\"session_ready\",\"run_engine\":\"x\"}";

    // A newer server could change the audio framing; refusing beats guessing.
    TEST_ASSERT_EQUAL_INT(-EPROTO, stt_session_json_parse_ready(newer, &ready));
    TEST_ASSERT_EQUAL_INT(-EPROTO, stt_session_json_parse_ready(missing, &ready));
}

void test_stt_session_ready_rejects_other_messages_and_malformed_json(void)
{
    TEST_ASSERT_EQUAL_INT(-EPROTO,
                          stt_session_json_parse_ready("{\"type\":\"error\",\"version\":1}",
                                                       &ready));
    TEST_ASSERT_EQUAL_INT(-EPROTO, stt_session_json_parse_ready("{\"type\":\"session_ready\"",
                                                                &ready));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_parse_ready(NULL, &ready));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_session_json_parse_ready("{\"type\":\"session_ready\"}", NULL));
}

void test_stt_session_ready_marks_a_run_config_that_did_not_fit(void)
{
    static char json[STT_SESSION_RUN_CONFIG_MAX + 256];
    size_t used;
    size_t i;

    used = (size_t)snprintf(json, sizeof(json),
                            "{\"type\":\"session_ready\",\"version\":1,\"run_config\":{\"k\":\"");
    for (i = 0U; i < STT_SESSION_RUN_CONFIG_MAX; i++)
    {
        json[used++] = 'x';
    }
    used += (size_t)snprintf(&json[used], sizeof(json) - used, "\"}}");
    json[used] = '\0';

    TEST_ASSERT_EQUAL_INT(0, stt_session_json_parse_ready(json, &ready));
    TEST_ASSERT_EQUAL_UINT8(1U, ready.run_config_truncated);
    TEST_ASSERT_EQUAL_size_t(STT_SESSION_RUN_CONFIG_MAX - 1U, strlen(ready.run_config));
}

// === error ======================================================================================================= //

void test_stt_session_error_flags_the_retryable_busy_rejection(void)
{
    // The server allows one GPU session; this is transient, not fatal.
    char const* const json = "{\"type\":\"error\",\"message\":\"server busy\",\"busy\":true}";

    TEST_ASSERT_EQUAL_INT(0, stt_session_json_parse_error(json, &error));
    TEST_ASSERT_EQUAL_STRING("server busy", error.message);
    TEST_ASSERT_EQUAL_UINT8(1U, error.busy);
}

void test_stt_session_error_defaults_busy_to_false(void)
{
    char const* const json = "{\"type\":\"error\",\"message\":\"backend not ready: loading\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_session_json_parse_error(json, &error));
    TEST_ASSERT_EQUAL_STRING("backend not ready: loading", error.message);
    TEST_ASSERT_EQUAL_UINT8(0U, error.busy);
}

void test_stt_session_error_rejects_other_messages(void)
{
    TEST_ASSERT_EQUAL_INT(-EPROTO,
                          stt_session_json_parse_error("{\"type\":\"pong\"}", &error));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_session_json_parse_error(NULL, &error));
}
