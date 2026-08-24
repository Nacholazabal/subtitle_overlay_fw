// Unit tests for the shared JSON scanning primitives (src/svc/stt/stt_json.c).
//
// stt_json is exercised indirectly by the session/transcript parsers, but its
// escape handling, UTF-8 truncation trimming and scalar validation have branches
// those callers never reach. This file drives them directly.

#include <errno.h>
#include <string.h>

#include "stt_json.h"
#include "unity.h"

// stt_json_parse_u32 delegates to the shared numeric parser; link it explicitly.
TEST_SOURCE_FILE("number_parse.c")

void setUp(void)
{
}

void tearDown(void)
{
}

// === stt_json_skip_whitespace ===

void test_skip_whitespace_advances_past_all_json_blanks(void)
{
    char const* json = " \t\r\n\"x\"";
    char const* cursor = json;

    stt_json_skip_whitespace(&cursor);

    TEST_ASSERT_EQUAL_CHAR('"', *cursor);
}

// === stt_json_parse_string: structural errors ===

void test_parse_string_requires_an_opening_quote(void)
{
    char const* json = "notaquote";
    char const* cursor = json;
    char dst[16];

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
}

void test_parse_string_rejects_unterminated_string(void)
{
    char const* json = "\"abc";
    char const* cursor = json;
    char dst[16];

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
}

void test_parse_string_rejects_raw_control_characters(void)
{
    // A literal control byte below 0x20 must be escaped in JSON.
    char const* json = "\"ab\x01\"";
    char const* cursor = json;
    char dst[16];

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
}

void test_parse_string_rejects_trailing_backslash(void)
{
    // Backslash immediately before the end of input: nothing left to escape.
    char const* json = "\"ab\\";
    char const* cursor = json;
    char dst[16];

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
}

void test_parse_string_rejects_unsupported_escape(void)
{
    // \u is not supported by this scanner.
    char const* json = "\"a\\u0041\"";
    char const* cursor = json;
    char dst[16];

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
}

// === stt_json_parse_string: escape handling ===

void test_parse_string_folds_whitespace_escapes_to_spaces(void)
{
    char const* json = "\"a\\nb\\tc\\rd\\fe\\bf\"";
    char const* cursor = json;
    char dst[32];

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
    TEST_ASSERT_EQUAL_STRING("a b c d e f", dst);
}

void test_parse_string_keeps_literal_escapes(void)
{
    char const* json = "\"q\\\"b\\\\s\\/e\"";
    char const* cursor = json;
    char dst[32];

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
    TEST_ASSERT_EQUAL_STRING("q\"b\\s/e", dst);
}

void test_parse_string_advances_cursor_past_closing_quote(void)
{
    char const* json = "\"ab\",next";
    char const* cursor = json;
    char dst[16];

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
    TEST_ASSERT_EQUAL_CHAR(',', *cursor);
}

// === stt_json_parse_string: empty / discard / truncation ===

void test_parse_string_accepts_empty_string_unless_required_non_empty(void)
{
    char const* json = "\"\"";
    char const* cursor = json;
    char dst[8];

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, NULL));
    TEST_ASSERT_EQUAL_STRING("", dst);

    cursor = json;
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_string(&cursor, dst, sizeof(dst), 1U, NULL));
}

void test_parse_string_discards_value_when_destination_is_null(void)
{
    char const* json = "\"discarded\",";
    char const* cursor = json;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, NULL, 0U, 0U, NULL));
    TEST_ASSERT_EQUAL_CHAR(',', *cursor);
}

void test_parse_string_reports_truncation_and_still_consumes_the_value(void)
{
    char const* json = "\"abcdefgh\",";
    char const* cursor = json;
    char dst[4];
    uint8_t truncated = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, &truncated));
    TEST_ASSERT_EQUAL_UINT8(1U, truncated);
    TEST_ASSERT_EQUAL_STRING("abc", dst);
    TEST_ASSERT_EQUAL_CHAR(',', *cursor);
}

void test_parse_string_clears_truncation_flag_when_value_fits(void)
{
    char const* json = "\"ab\"";
    char const* cursor = json;
    char dst[16];
    uint8_t truncated = 1U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, &truncated));
    TEST_ASSERT_EQUAL_UINT8(0U, truncated);
}

// === stt_json_parse_string: UTF-8 boundary trimming ===
//
// Spanish subtitles are full of multi-byte code points, so a byte-boundary
// truncation must not leave half of one behind.

void test_parse_string_trims_split_two_byte_code_point(void)
{
    // "ñ" is 0xC3 0xB1. dst holds 2 payload bytes, so 'a' and the 0xC3 lead fit
    // but the 0xB1 continuation does not.
    char const* json = "\"a\xC3\xB1\"";
    char const* cursor = json;
    char dst[3];
    uint8_t truncated = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, &truncated));
    TEST_ASSERT_EQUAL_UINT8(1U, truncated);
    // The dangling 0xC3 lead byte is dropped rather than emitted alone.
    TEST_ASSERT_EQUAL_STRING("a", dst);
}

void test_parse_string_keeps_complete_code_point_and_drops_the_split_one(void)
{
    // Three payload bytes: 'a' plus a whole "ñ" fit, the second "ñ" is dropped
    // entirely rather than leaving a half-written code point behind.
    char const* json = "\"a\xC3\xB1\xC3\xB1\"";
    char const* cursor = json;
    char dst[4];
    uint8_t truncated = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, &truncated));
    TEST_ASSERT_EQUAL_UINT8(1U, truncated);
    TEST_ASSERT_EQUAL_STRING("a\xC3\xB1", dst);
}

void test_parse_string_keeps_whole_two_byte_code_point(void)
{
    char const* json = "\"\xC3\xB1zzz\"";
    char const* cursor = json;
    char dst[3]; // exactly fits the 2-byte code point + NUL
    uint8_t truncated = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, &truncated));
    TEST_ASSERT_EQUAL_UINT8(1U, truncated);
    TEST_ASSERT_EQUAL_STRING("\xC3\xB1", dst);
}

void test_parse_string_trims_split_three_byte_code_point(void)
{
    // "€" is 0xE2 0x82 0xAC; only two of its three bytes fit.
    char const* json = "\"\xE2\x82\xAC\"";
    char const* cursor = json;
    char dst[3];
    uint8_t truncated = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, &truncated));
    TEST_ASSERT_EQUAL_UINT8(1U, truncated);
    TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_parse_string_trims_split_four_byte_code_point(void)
{
    // U+1F600 is 0xF0 0x9F 0x98 0x80; only three of its four bytes fit.
    char const* json = "\"\xF0\x9F\x98\x80\"";
    char const* cursor = json;
    char dst[4];
    uint8_t truncated = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_string(&cursor, dst, sizeof(dst), 0U, &truncated));
    TEST_ASSERT_EQUAL_UINT8(1U, truncated);
    TEST_ASSERT_EQUAL_STRING("", dst);
}

// === stt_json_scalar_span ===

void test_scalar_span_rejects_null_arguments(void)
{
    char const* json = "42";
    char const* cursor = json;
    char const* start = NULL;
    size_t length = 0U;

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_scalar_span(NULL, &start, &length));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_scalar_span(&cursor, NULL, &length));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_scalar_span(&cursor, &start, NULL));
}

void test_scalar_span_stops_at_array_and_object_terminators(void)
{
    char const* json = "12]";
    char const* cursor = json;
    char const* start = NULL;
    size_t length = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_scalar_span(&cursor, &start, &length));
    TEST_ASSERT_EQUAL_size_t(2U, length);
    TEST_ASSERT_EQUAL_CHAR(']', *cursor);

    cursor = "7}";
    TEST_ASSERT_EQUAL_INT(0, stt_json_scalar_span(&cursor, &start, &length));
    TEST_ASSERT_EQUAL_size_t(1U, length);
    TEST_ASSERT_EQUAL_CHAR('}', *cursor);
}

// === stt_json_parse_bool ===

void test_parse_bool_accepts_true_and_false_literals(void)
{
    char const* cursor = "true,";
    uint8_t value = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_bool(&cursor, &value));
    TEST_ASSERT_EQUAL_UINT8(1U, value);

    cursor = "false,";
    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_bool(&cursor, &value));
    TEST_ASSERT_EQUAL_UINT8(0U, value);
}

void test_parse_bool_accepts_legacy_numeric_form(void)
{
    // Older senders emit 0/1 instead of false/true.
    char const* cursor = "1,";
    uint8_t value = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_bool(&cursor, &value));
    TEST_ASSERT_EQUAL_UINT8(1U, value);

    cursor = "0,";
    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_bool(&cursor, &value));
    TEST_ASSERT_EQUAL_UINT8(0U, value);
}

void test_parse_bool_rejects_other_tokens(void)
{
    char const* cursor = "maybe,";
    uint8_t value = 0U;

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_bool(&cursor, &value));

    cursor = "2,";
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_bool(&cursor, &value));
}

// === stt_json_parse_double ===

void test_parse_double_accepts_signed_and_fractional_values(void)
{
    char const* cursor = "1.5,";
    double value = 0.0;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_double(&cursor, &value));
    TEST_ASSERT_TRUE(value > 1.49 && value < 1.51);

    cursor = "-2.25}";
    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_double(&cursor, &value));
    TEST_ASSERT_TRUE(value < -2.24 && value > -2.26);
}

void test_parse_double_rejects_non_numeric_leading_byte(void)
{
    char const* cursor = "abc,";
    double value = 0.0;

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_double(&cursor, &value));
}

void test_parse_double_rejects_null_value_pointer(void)
{
    char const* cursor = "1.0,";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_double(&cursor, NULL));
}

void test_parse_double_rejects_token_longer_than_scratch_buffer(void)
{
    // STT_JSON_TOKEN_MAX-byte scratch buffer: an over-long numeric token is
    // rejected rather than truncated into a different number.
    char long_token[STT_JSON_TOKEN_MAX + 8U];
    char const* cursor;

    memset(long_token, '1', sizeof(long_token) - 1U);
    long_token[sizeof(long_token) - 1U] = '\0';
    cursor = long_token;

    double value = 0.0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_double(&cursor, &value));
}

// === stt_json_parse_u32 ===

void test_parse_u32_accepts_plain_integers(void)
{
    char const* cursor = "1234,";
    uint32_t value = 0U;

    TEST_ASSERT_EQUAL_INT(0, stt_json_parse_u32(&cursor, &value));
    TEST_ASSERT_EQUAL_UINT32(1234U, value);
    TEST_ASSERT_EQUAL_CHAR(',', *cursor);
}

void test_parse_u32_rejects_out_of_range_and_malformed_tokens(void)
{
    char const* cursor = "99999999999,";
    uint32_t value = 0U;

    TEST_ASSERT_EQUAL_INT(-ERANGE, stt_json_parse_u32(&cursor, &value));

    cursor = "-1,";
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_parse_u32(&cursor, &value));
}

// === stt_json_skip_value ===

void test_skip_value_skips_nested_arrays_and_objects(void)
{
    // The server sends fields the firmware does not consume, e.g. att_context_size.
    char const* cursor = "[56,6],\"next\"";

    TEST_ASSERT_EQUAL_INT(0, stt_json_skip_value(&cursor));
    TEST_ASSERT_EQUAL_CHAR(',', *cursor);

    cursor = "{\"a\":{\"b\":[1,2]}},\"next\"";
    TEST_ASSERT_EQUAL_INT(0, stt_json_skip_value(&cursor));
    TEST_ASSERT_EQUAL_CHAR(',', *cursor);
}

void test_skip_value_skips_strings_and_scalars(void)
{
    char const* cursor = "\"text with , comma\",1";

    TEST_ASSERT_EQUAL_INT(0, stt_json_skip_value(&cursor));
    TEST_ASSERT_EQUAL_CHAR(',', *cursor);

    cursor = "3.5}";
    TEST_ASSERT_EQUAL_INT(0, stt_json_skip_value(&cursor));
    TEST_ASSERT_EQUAL_CHAR('}', *cursor);
}

void test_skip_value_rejects_unterminated_container(void)
{
    char const* cursor = "[1,2";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_json_skip_value(&cursor));
}
