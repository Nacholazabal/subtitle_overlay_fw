/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file test_trace.c
/// @brief Unit tests for the profiling trace writer
///
/// The writer is the one component whose own defects silently corrupt every
/// measurement taken with it, so these tests assert the properties a capture is
/// later accepted on: one whole JSON line per event, valid UTF-8, a bounded
/// file, and no crash when profiling is off or misused.
///

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "trace.h"
#include "unity.h"

// === Test fixtures =============================================================================================== //

#define TEST_TRACE_PATH "build/test_trace_output.jsonl"
#define TEST_FILE_BUFFER (512U * 1024U)

static char file_text[TEST_FILE_BUFFER];
static trace_ctx_t* tracer;

/** @brief Read the whole trace file into file_text; returns the byte count. */
static size_t read_trace_file(void)
{
    FILE* const handle = fopen(TEST_TRACE_PATH, "rb");
    size_t read_bytes = 0U;

    memset(file_text, 0, sizeof(file_text));
    if (handle == NULL)
    {
        return 0U;
    }

    read_bytes = fread(file_text, 1U, sizeof(file_text) - 1U, handle);
    (void)fclose(handle);
    file_text[read_bytes] = '\0';
    return read_bytes;
}

/** @brief Count how many lines contain @p needle. */
static uint32_t count_lines_with(char const* const needle)
{
    uint32_t found = 0U;
    char const* cursor = file_text;

    while ((cursor = strstr(cursor, needle)) != NULL)
    {
        found++;
        cursor += strlen(needle);
    }

    return found;
}

/** @brief Number of newline-terminated lines in the file. */
static uint32_t count_lines(void)
{
    uint32_t lines = 0U;
    size_t index;

    for (index = 0U; file_text[index] != '\0'; index++)
    {
        if (file_text[index] == '\n')
        {
            lines++;
        }
    }

    return lines;
}

/**
 * @brief Assert every line is a self-contained JSON object.
 *
 * This is the property interleaved writers would break, and the one that makes
 * the difference between a usable capture and an unparseable file.
 */
static void assert_every_line_is_whole(void)
{
    size_t index = 0U;

    while (file_text[index] != '\0')
    {
        size_t const start = index;
        size_t end;

        while ((file_text[index] != '\n') && (file_text[index] != '\0'))
        {
            index++;
        }
        end = index;

        TEST_ASSERT_TRUE_MESSAGE(end > start, "empty line in trace output");
        TEST_ASSERT_EQUAL_MESSAGE('{', file_text[start], "line does not start with '{'");
        TEST_ASSERT_EQUAL_MESSAGE('}', file_text[end - 1U], "line does not end with '}'");

        if (file_text[index] == '\n')
        {
            index++;
        }
    }
}

static trace_ctx_t* open_tracer(uint64_t const max_bytes)
{
    trace_config_t config;

    memset(&config, 0, sizeof(config));
    config.output_path = TEST_TRACE_PATH;
    config.source = "board";
    config.run_id = "run-under-test";
    config.build_id = "test-build";
    config.max_bytes = max_bytes;
    config.flush_interval_ms = 1U;

    return trace_init(&config);
}

void setUp(void)
{
    tracer = NULL;
    memset(file_text, 0, sizeof(file_text));
}

void tearDown(void)
{
    if (tracer != NULL)
    {
        trace_close(tracer);
        tracer = NULL;
    }
    (void)remove(TEST_TRACE_PATH);
}

// === Tests ======================================================================================================= //

void test_trace_init_writes_process_metadata_and_a_clock_anchor(void)
{
    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_flush(tracer);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();

    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"process_name\""));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"trace_start\""));
    // Without both anchors the merge cannot place this file on a UTC timeline.
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"clock_monotonic_ns\""));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"clock_realtime_ns\""));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"run_id\":\"run-under-test\""));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"build_id\":\"test-build\""));
}

void test_trace_close_reports_its_own_drop_counters(void)
{
    trace_arg_t const args[] = {TRACE_U64("value", 7U)};

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_instant(tracer, "sample", args, 1U);
    trace_close(tracer);
    tracer = NULL;

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"trace_stats\""));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"dropped_truncated\":0"));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"write_errors\":0"));
}

void test_trace_escapes_quotes_backslashes_and_control_characters(void)
{
    trace_arg_t const args[] = {TRACE_STR("reason", "he said \"stop\"\\now\nline\ttab")};

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_instant(tracer, "escaping", args, 1U);
    trace_flush(tracer);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    // Assert the exact escaped form rather than counting fragments, which would
    // also match the "n" that follows an escaped backslash.
    TEST_ASSERT_EQUAL_UINT32(
        1U, count_lines_with("\"reason\":\"he said \\\"stop\\\"\\\\now\\nline\\ttab\""));
    // process_name + trace_start + this event. A raw newline inside the value
    // would have split the event across two lines and pushed the count to four.
    TEST_ASSERT_EQUAL_UINT32(3U, count_lines());
}

void test_trace_keeps_complete_utf8_and_replaces_broken_sequences(void)
{
    // "Española" plus a lead byte with no continuation, the exact shape that
    // corrupted eight lines of the 20260907-0055 capture.
    trace_arg_t const args[] = {
        TRACE_STR("complete", "Espa\xc3\xb1ola"),
        TRACE_STR("truncated", "abc\xc3"),
        TRACE_STR("bad_continuation", "abc\xe2\x28\xa1"),
    };

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_instant(tracer, "utf8", args, 3U);
    trace_flush(tracer);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();

    // A complete code point survives untouched.
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"complete\":\"Espa\xc3\xb1ola\""));
    // A lead byte with no continuation becomes one replacement character.
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"truncated\":\"abc\\ufffd\""));
    // The malformed three-byte sequence yields two: the bad lead, then the
    // orphan continuation byte that follows the valid '(' in the middle.
    TEST_ASSERT_EQUAL_UINT32(
        1U, count_lines_with("\"bad_continuation\":\"abc\\ufffd(\\ufffd\""));
}

void test_trace_truncates_a_long_string_on_a_code_point_boundary(void)
{
    char long_text[512];
    trace_arg_t const args[] = {TRACE_STR("text", long_text)};
    size_t index;
    size_t bytes;

    // Fill past TRACE_STR_ARG_MAX with two-byte code points so a byte-wise cut
    // would land in the middle of one.
    for (index = 0U; index < (sizeof(long_text) - 3U); index += 2U)
    {
        long_text[index] = (char)0xC3;
        long_text[index + 1U] = (char)0xB1;
    }
    long_text[sizeof(long_text) - 1U] = '\0';

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_instant(tracer, "truncation", args, 1U);
    trace_flush(tracer);

    bytes = read_trace_file();
    TEST_ASSERT_TRUE(bytes > 0U);
    assert_every_line_is_whole();
    // No replacement character means nothing was cut mid-sequence.
    TEST_ASSERT_EQUAL_UINT32(0U, count_lines_with("\\ufffd"));
}

void test_trace_slice_emits_one_complete_event_with_a_duration(void)
{
    uint64_t const started = trace_now_ns();
    trace_arg_t const args[] = {TRACE_I64("status", 0)};

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_slice(tracer, "subtitle_render", started, args, 1U);
    trace_flush(tracer);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    // Phase X is self-contained: there is no separate end event to lose.
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"subtitle_render\",\"ph\":\"X\""));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"dur\":"));
    TEST_ASSERT_EQUAL_UINT32(0U, count_lines_with("\"ph\":\"B\""));
    TEST_ASSERT_EQUAL_UINT32(0U, count_lines_with("\"ph\":\"E\""));
}

void test_trace_counter_uses_the_counter_phase(void)
{
    trace_arg_t const args[] = {TRACE_U64("audio_txq_depth", 3U), TRACE_U64("dropped", 0U)};

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_counter(tracer, "pipeline_health", args, 2U);
    trace_flush(tracer);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"pipeline_health\",\"ph\":\"C\""));
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"audio_txq_depth\":3"));
}

void test_trace_stops_at_the_size_cap_and_counts_the_dropped_events(void)
{
    trace_stats_t stats;
    uint32_t index;

    tracer = open_tracer(4096U);
    TEST_ASSERT_NOT_NULL(tracer);

    for (index = 0U; index < 2000U; index++)
    {
        trace_arg_t const args[] = {TRACE_U64("index", index)};

        trace_instant(tracer, "spam", args, 1U);
    }
    trace_flush(tracer);

    trace_get_stats(tracer, &stats);
    TEST_ASSERT_EQUAL_UINT8(1U, stats.full);
    TEST_ASSERT_TRUE(stats.dropped_full > 0U);
    TEST_ASSERT_TRUE(stats.events_written < 2000U);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    // The capture must say it stopped, instead of silently ending.
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"trace_full\""));
}

void test_trace_drops_an_oversized_event_instead_of_writing_a_partial_line(void)
{
    char keys[TRACE_ARG_MAX][32];
    trace_arg_t args[TRACE_ARG_MAX];
    char filler[TRACE_STR_ARG_MAX + 8U];
    trace_stats_t stats;
    size_t index;

    memset(filler, 'x', sizeof(filler) - 1U);
    filler[sizeof(filler) - 1U] = '\0';

    for (index = 0U; index < TRACE_ARG_MAX; index++)
    {
        (void)snprintf(keys[index], sizeof(keys[index]), "a_very_long_argument_key_%02u",
                       (unsigned)index);
        args[index].key = keys[index];
        args[index].type = TRACE_ARG_TYPE_STR;
        args[index].value.str = filler;
    }

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);
    trace_instant(tracer, "oversized", args, TRACE_ARG_MAX);
    trace_flush(tracer);

    trace_get_stats(tracer, &stats);
    TEST_ASSERT_EQUAL_UINT64(1U, stats.dropped_truncated);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    TEST_ASSERT_EQUAL_UINT32(0U, count_lines_with("oversized"));
}

void test_trace_entry_points_tolerate_a_null_context(void)
{
    trace_arg_t const args[] = {TRACE_U64("value", 1U)};
    trace_stats_t stats;

    // Profiling must degrade to nothing rather than take the firmware down when
    // the trace file could not be opened.
    trace_instant(NULL, "ignored", args, 1U);
    trace_slice(NULL, "ignored", trace_now_ns(), args, 1U);
    trace_counter(NULL, "ignored", args, 1U);
    trace_thread_register(NULL, "ignored");
    trace_flush(NULL);
    trace_close(NULL);

    memset(&stats, 0xAA, sizeof(stats));
    trace_get_stats(NULL, &stats);
    TEST_ASSERT_EQUAL_UINT64(0U, stats.events_written);
    TEST_ASSERT_EQUAL_UINT8(0U, stats.full);
}

void test_trace_tolerates_a_null_name_and_null_arguments(void)
{
    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);

    trace_instant(tracer, NULL, NULL, 0U);
    trace_instant(tracer, "no_args", NULL, 0U);
    trace_thread_register(tracer, NULL);
    trace_flush(tracer);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"no_args\""));
}

// --- concurrency ---------------------------------------------------------- //

#define TEST_WRITER_THREADS (4U)
#define TEST_EVENTS_PER_THREAD (250U)

static void* writer_thread(void* const arg)
{
    char const* const name = (char const*)arg;
    uint32_t index;

    trace_thread_register(tracer, name);

    for (index = 0U; index < TEST_EVENTS_PER_THREAD; index++)
    {
        uint64_t const started = trace_now_ns();
        trace_arg_t const instant_args[] = {TRACE_U64("index", index), TRACE_STR("who", name)};
        trace_arg_t const slice_args[] = {TRACE_I64("status", -5)};

        trace_instant(tracer, "concurrent_instant", instant_args, 2U);
        trace_slice(tracer, "concurrent_slice", started, slice_args, 1U);
    }

    return NULL;
}

void test_trace_concurrent_writers_never_interleave_a_line(void)
{
    static char const* const names[TEST_WRITER_THREADS] = {"qpc-main", "usb-capture",
                                                           "stt-network", "extra"};
    pthread_t threads[TEST_WRITER_THREADS];
    trace_stats_t stats;
    uint32_t index;

    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);

    for (index = 0U; index < TEST_WRITER_THREADS; index++)
    {
        TEST_ASSERT_EQUAL_INT(
            0, pthread_create(&threads[index], NULL, writer_thread, (void*)names[index]));
    }
    for (index = 0U; index < TEST_WRITER_THREADS; index++)
    {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[index], NULL));
    }
    trace_flush(tracer);

    trace_get_stats(tracer, &stats);
    TEST_ASSERT_EQUAL_UINT64(0U, stats.dropped_full);
    TEST_ASSERT_EQUAL_UINT64(0U, stats.dropped_truncated);
    TEST_ASSERT_EQUAL_UINT64(0U, stats.write_errors);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();

    TEST_ASSERT_EQUAL_UINT32(TEST_WRITER_THREADS * TEST_EVENTS_PER_THREAD,
                             count_lines_with("\"name\":\"concurrent_instant\""));
    TEST_ASSERT_EQUAL_UINT32(TEST_WRITER_THREADS * TEST_EVENTS_PER_THREAD,
                             count_lines_with("\"name\":\"concurrent_slice\""));
    // One metadata record per worker: the tracks Perfetto will show.
    TEST_ASSERT_EQUAL_UINT32(TEST_WRITER_THREADS, count_lines_with("\"name\":\"thread_name\""));
}

void test_trace_registers_each_thread_track_only_once(void)
{
    tracer = open_tracer(0U);
    TEST_ASSERT_NOT_NULL(tracer);

    trace_thread_register(tracer, "qpc-main");
    trace_thread_register(tracer, "qpc-main");
    trace_thread_register(tracer, "qpc-main");
    trace_flush(tracer);

    TEST_ASSERT_TRUE(read_trace_file() > 0U);
    assert_every_line_is_whole();
    TEST_ASSERT_EQUAL_UINT32(1U, count_lines_with("\"name\":\"thread_name\""));
}
