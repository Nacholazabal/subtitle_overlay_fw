#include <string.h>

#include "unity.h"
#include "errorno.h"
#include "subtitle_text_sanitize.h"

static char out[64];

void setUp(void)
{
    memset(out, 0xAA, sizeof(out));
}

void tearDown(void)
{}

void test_sanitize_passes_printable_ascii_through(void)
{
    TEST_ASSERT_EQUAL_INT(0, subtitle_text_sanitize("hola mundo 123.", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("hola mundo 123.", out);
}

void test_sanitize_folds_lowercase_accents_enie_and_dieresis(void)
{
    // "áéíóú ñ ü"
    TEST_ASSERT_EQUAL_INT(0, subtitle_text_sanitize("\xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xBA"
                                                    " \xC3\xB1 \xC3\xBC",
                                                    out,
                                                    sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("aeiou n u", out);
}

void test_sanitize_folds_uppercase_accents_enie_and_dieresis(void)
{
    // "ÁÉÍÓÚ ÑÜ"
    TEST_ASSERT_EQUAL_INT(0, subtitle_text_sanitize("\xC3\x81\xC3\x89\xC3\x8D\xC3\x93\xC3\x9A"
                                                    " \xC3\x91\xC3\x9C",
                                                    out,
                                                    sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("AEIOU NU", out);
}

void test_sanitize_maps_inverted_marks_to_space_and_keeps_ascii_marks(void)
{
    // "¿Hola? ¡Hey!"
    TEST_ASSERT_EQUAL_INT(0, subtitle_text_sanitize("\xC2\xBF" "Hola? \xC2\xA1" "Hey!",
                                                    out,
                                                    sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(" Hola?  Hey!", out);
}

void test_sanitize_maps_typographic_quotes_dashes_and_ellipsis(void)
{
    // "“hola” – ‘x’ …"  and a straight ASCII double quote
    TEST_ASSERT_EQUAL_INT(0,
                          subtitle_text_sanitize("\xE2\x80\x9C" "hola" "\xE2\x80\x9D"
                                                 " \xE2\x80\x93 \xE2\x80\x98x\xE2\x80\x99"
                                                 " \xE2\x80\xA6 \"q\"",
                                                 out,
                                                 sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("'hola' - 'x' . 'q'", out);
}

void test_sanitize_collapses_unknown_multibyte_to_single_space(void)
{
    // unmapped 2-byte (ÿ = C3 BF), 3-byte, and a 4-byte emoji each become ONE space.
    TEST_ASSERT_EQUAL_INT(0,
                          subtitle_text_sanitize("a" "\xC3\xBF" "b" "\xE2\x82\xAC" "c"
                                                 "\xF0\x9F\x98\x80" "d",
                                                 out,
                                                 sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a b c d", out);
}

void test_sanitize_collapses_stray_continuation_byte_to_space(void)
{
    TEST_ASSERT_EQUAL_INT(0, subtitle_text_sanitize("a" "\x80" "b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a b", out);
}

void test_sanitize_converts_control_characters_to_space(void)
{
    TEST_ASSERT_EQUAL_INT(0, subtitle_text_sanitize("a\tb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a b", out);
}

void test_sanitize_truncates_safely_and_terminates(void)
{
    char small[4];

    memset(small, 0xAA, sizeof(small));
    TEST_ASSERT_EQUAL_INT(0, subtitle_text_sanitize("abcdef", small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT(3U, strlen(small));
    TEST_ASSERT_EQUAL_STRING("abc", small);
    TEST_ASSERT_EQUAL_CHAR('\0', small[3]);
}

void test_sanitize_does_not_split_multibyte_when_truncating(void)
{
    char small[3];

    // Input is 'a', á(->a), é(->e); out holds only 2 chars + NUL, so 'é' is dropped
    // cleanly without leaving a partial multibyte fragment.
    memset(small, 0xAA, sizeof(small));
    TEST_ASSERT_EQUAL_INT(0,
                          subtitle_text_sanitize("a\xC3\xA1\xC3\xA9", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("aa", small);
    TEST_ASSERT_EQUAL_CHAR('\0', small[2]);
}

void test_sanitize_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_text_sanitize(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_text_sanitize("x", NULL, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_text_sanitize("x", out, 0U));
}
