/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_json.c
/// @brief Cursor-based scanning for the flat JSON objects the STT server sends
///

// === Headers files inclusions ==================================================================================== //

#include "stt_json.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "number_parse.h"

// === Macros definitions ========================================================================================== //

#define STT_JSON_BOOL_TRUE_LEN  (4U)
#define STT_JSON_BOOL_FALSE_LEN (5U)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static int json_skip_value_at(char const** cursor, uint32_t depth);
static int json_skip_container(char const** cursor, uint32_t depth);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Skip one JSON array or object, including anything nested inside it.
 *
 * The streaming server sends fields the firmware ignores but must tolerate, such
 * as `"att_context_size":[56,6]`. Recursion is bounded by
 * ::STT_JSON_MAX_DEPTH so a hostile line cannot exhaust the stack.
 *
 * @param cursor Cursor positioned on the opening '[' or '{'.
 * @param depth Current nesting depth.
 * @return 0 on success, or -EINVAL on malformed or too deeply nested input.
 */
static int json_skip_container(char const** const cursor, uint32_t const depth)
{
    char const opening = **cursor;
    char const closing = (opening == '[') ? ']' : '}';
    uint8_t done = 0U;
    int status = 0;

    if (depth >= STT_JSON_MAX_DEPTH)
    {
        return -EINVAL;
    }

    (*cursor)++;
    stt_json_skip_whitespace(cursor);
    if (**cursor == closing)
    {
        (*cursor)++;
        return 0;
    }

    while ((status == 0) && (done == 0U))
    {
        if (opening == '{')
        {
            status = stt_json_parse_string(cursor, NULL, 0U, 0U, NULL);
            stt_json_skip_whitespace(cursor);
            if ((status == 0) && (**cursor != ':'))
            {
                status = -EINVAL;
            }
            if (status == 0)
            {
                (*cursor)++;
                stt_json_skip_whitespace(cursor);
            }
        }

        if (status == 0)
        {
            status = json_skip_value_at(cursor, depth + 1U);
        }
        if (status != 0)
        {
            break;
        }

        stt_json_skip_whitespace(cursor);
        if (**cursor == ',')
        {
            (*cursor)++;
            stt_json_skip_whitespace(cursor);
        }
        else if (**cursor == closing)
        {
            (*cursor)++;
            done = 1U;
        }
        else
        {
            status = -EINVAL;
        }
    }

    return status;
}

/** @brief Skip one valid JSON value: scalar, string, array or object. */
static int json_skip_value_at(char const** const cursor, uint32_t const depth)
{
    char const* start;
    size_t length;
    double ignored;

    if ((**cursor == '[') || (**cursor == '{'))
    {
        return json_skip_container(cursor, depth);
    }

    if (**cursor == '"')
    {
        return stt_json_parse_string(cursor, NULL, 0U, 0U, NULL);
    }

    if (stt_json_scalar_span(cursor, &start, &length) != 0)
    {
        return -EINVAL;
    }
    if (((length == 4U)
         && ((memcmp(start, "true", length) == 0) || (memcmp(start, "null", length) == 0)))
        || ((length == 5U) && (memcmp(start, "false", length) == 0)))
    {
        return 0;
    }

    *cursor = start;
    return stt_json_parse_double(cursor, &ignored);
}

// === Public function implementation ============================================================================== //

/** @brief Advance over JSON whitespace. */
void stt_json_skip_whitespace(char const** const cursor)
{
    while (isspace((unsigned char)**cursor))
    {
        (*cursor)++;
    }
}

/**
 * @brief Parse one JSON string, decoding only escapes used by the trusted sender contract.
 * @return 0 on success or -EINVAL for malformed/unsupported escapes.
 */
int stt_json_parse_string(char const** const cursor,
                             char* const dst,
                             size_t dst_size,
                             uint8_t require_non_empty,
                             uint8_t* const truncated)
{
    size_t out = 0U;
    uint8_t non_empty = 0U;
    uint8_t was_truncated = 0U;

    if ((cursor == NULL) || (*cursor == NULL) || (**cursor != '"')
        || ((dst != NULL) && (dst_size == 0U)))
    {
        return -EINVAL;
    }

    (*cursor)++;
    while ((**cursor != '\0') && (**cursor != '"'))
    {
        unsigned char const raw = (unsigned char)**cursor;
        char ch;

        if (raw < 0x20U)
        {
            return -EINVAL;
        }

        ch = **cursor;
        (*cursor)++;
        non_empty = 1U;
        if (ch == '\\')
        {
            ch = **cursor;
            if (ch == '\0')
            {
                return -EINVAL;
            }
            (*cursor)++;

            switch (ch)
            {
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                ch = ' ';
                break;

            case '"':
            case '\\':
            case '/':
                break;

            default:
                return -EINVAL;
            }
        }

        if (dst != NULL)
        {
            if ((out + 1U) < dst_size)
            {
                dst[out++] = ch;
            }
            else
            {
                was_truncated = 1U;
            }
        }
    }

    if (**cursor != '"')
    {
        return -EINVAL;
    }
    (*cursor)++;

    if (dst != NULL)
    {
        // A byte-boundary truncation can split a multi-byte UTF-8 code point.
        // Trim any incomplete trailing sequence so the field always ends on a
        // whole code point instead of a mangled byte.
        if ((was_truncated != 0U) && (out > 0U))
        {
            size_t i = out;

            while ((i > 0U) && (((unsigned char)dst[i - 1U] & 0xC0U) == 0x80U))
            {
                i--; // skip UTF-8 continuation bytes (10xxxxxx)
            }

            if (i > 0U)
            {
                size_t const lead_idx = i - 1U;
                unsigned char const lead = (unsigned char)dst[lead_idx];
                size_t expected;

                if ((lead & 0x80U) == 0x00U)
                {
                    expected = 1U;
                }
                else if ((lead & 0xE0U) == 0xC0U)
                {
                    expected = 2U;
                }
                else if ((lead & 0xF0U) == 0xE0U)
                {
                    expected = 3U;
                }
                else if ((lead & 0xF8U) == 0xF0U)
                {
                    expected = 4U;
                }
                else
                {
                    expected = 0U; // invalid lead byte
                }

                if ((expected == 0U) || ((out - lead_idx) < expected))
                {
                    out = lead_idx; // drop the incomplete/invalid trailing code point
                }
            }
        }

        dst[out] = '\0';
    }
    if (truncated != NULL)
    {
        *truncated = was_truncated;
    }

    return ((require_non_empty == 0U) || (non_empty != 0U)) ? 0 : -EINVAL;
}

/** @brief Return and consume a non-string scalar token span. */
int stt_json_scalar_span(char const** const cursor,
                            char const** const start,
                            size_t* const length)
{
    char const* end;

    if ((cursor == NULL) || (*cursor == NULL) || (start == NULL) || (length == NULL))
    {
        return -EINVAL;
    }

    *start = *cursor;
    end = *cursor;
    // ']' terminates a token just like '}' so scalars inside arrays end cleanly.
    while ((*end != '\0') && (*end != ',') && (*end != '}') && (*end != ']')
           && (isspace((unsigned char)*end) == 0))
    {
        end++;
    }

    *length = (size_t)(end - *start);
    if (*length == 0U)
    {
        return -EINVAL;
    }

    *cursor = end;
    return 0;
}

/** @brief Parse a uint32 JSON scalar. */
int stt_json_parse_u32(char const** const cursor, uint32_t* const value)
{
    char const* start;
    size_t length;
    int status = stt_json_scalar_span(cursor, &start, &length);

    return (status == 0) ? number_parse_u32(start, length, 0U, UINT32_MAX, value) : status;
}

/** @brief Parse a boolean JSON scalar, retaining 0/1 compatibility. */
int stt_json_parse_bool(char const** const cursor, uint8_t* const value)
{
    char const* start;
    size_t length;
    int status = stt_json_scalar_span(cursor, &start, &length);

    if ((status == 0) && (length == STT_JSON_BOOL_TRUE_LEN)
        && (memcmp(start, "true", length) == 0))
    {
        *value = 1U;
        return 0;
    }
    if ((status == 0) && (length == STT_JSON_BOOL_FALSE_LEN)
        && (memcmp(start, "false", length) == 0))
    {
        *value = 0U;
        return 0;
    }
    if ((status == 0) && (length == 1U) && ((start[0] == '0') || (start[0] == '1')))
    {
        *value = (start[0] == '1') ? 1U : 0U;
        return 0;
    }

    return -EINVAL;
}

/** @brief Parse a finite floating-point JSON scalar with exact token consumption. */
int stt_json_parse_double(char const** const cursor, double* const value)
{
    char const* start;
    size_t length;
    char token[STT_JSON_TOKEN_MAX];
    char* end = NULL;
    double parsed;
    int status = stt_json_scalar_span(cursor, &start, &length);

    if ((status != 0) || (value == NULL) || (length >= sizeof(token))
        || ((start[0] != '-') && ((start[0] < '0') || (start[0] > '9'))))
    {
        return -EINVAL;
    }

    memcpy(token, start, length);
    token[length] = '\0';
    errno = 0;
    parsed = strtod(token, &end);
    if ((end == token) || (*end != '\0') || (errno == ERANGE) || (isfinite(parsed) == 0))
    {
        return -EINVAL;
    }

    *value = parsed;
    return 0;
}

/** @brief Skip one value of an unknown top-level field. */
int stt_json_skip_value(char const** const cursor)
{
    return json_skip_value_at(cursor, 0U);
}
// === End of documentation ======================================================================================== //
