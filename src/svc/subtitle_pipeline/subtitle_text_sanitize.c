/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file subtitle_text_sanitize.c
/// @brief UTF-8 normalizer for the supported subtitle character set
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_text_sanitize.h"

#include <stdint.h>
#include <string.h>

#include "errorno.h"

// === Macros definitions ========================================================================================== //

#define SANITIZE_ASCII_MIN (0x20U)
#define SANITIZE_ASCII_MAX (0x7EU)

// === Private function declarations =============================================================================== //

static uint8_t is_continuation(unsigned char byte);
static uint8_t encoded_length(unsigned char lead);
static uint8_t is_supported_spanish(unsigned char lead, unsigned char continuation);
static char normalize_ascii(unsigned char byte);
static void emit_bytes(char* out, size_t out_size, size_t* index, char const* bytes, size_t count);

// === Private function implementation ============================================================================= //

static uint8_t is_continuation(unsigned char const byte)
{
    return ((byte >= 0x80U) && (byte <= 0xBFU)) ? 1U : 0U;
}

static uint8_t encoded_length(unsigned char const lead)
{
    if ((lead >= 0xC2U) && (lead <= 0xDFU))
    {
        return 2U;
    }
    if ((lead >= 0xE0U) && (lead <= 0xEFU))
    {
        return 3U;
    }
    if ((lead >= 0xF0U) && (lead <= 0xF4U))
    {
        return 4U;
    }
    return 1U;
}

static uint8_t is_supported_spanish(unsigned char const lead, unsigned char const continuation)
{
    if ((lead == 0xC2U) && ((continuation == 0xA1U) || (continuation == 0xBFU)))
    {
        return 1U;
    }

    if (lead != 0xC3U)
    {
        return 0U;
    }

    switch (continuation)
    {
    case 0x81U: case 0x89U: case 0x8DU: case 0x91U: case 0x93U: case 0x9AU: case 0x9CU:
    case 0xA1U: case 0xA9U: case 0xADU: case 0xB1U: case 0xB3U: case 0xBAU: case 0xBCU:
        return 1U;
    default:
        return 0U;
    }
}

static char normalize_ascii(unsigned char const byte)
{
    if (byte < SANITIZE_ASCII_MIN)
    {
        return ' ';
    }
    if (byte == (unsigned char)'"')
    {
        return '\'';
    }
    return (char)byte;
}

static void emit_bytes(char* const out,
                       size_t const out_size,
                       size_t* const index,
                       char const* const bytes,
                       size_t const count)
{
    if ((*index + count) < out_size)
    {
        memcpy(&out[*index], bytes, count);
        *index += count;
    }
}

// === Public function implementation ============================================================================== //

/**
 * @brief Normalize UTF-8 subtitle text to the characters available in the generated font.
 *
 * Printable ASCII and supported Spanish letters and punctuation are preserved. Curly quotes,
 * dashes, and ellipses are converted to their printable ASCII equivalents. Unsupported or
 * malformed UTF-8 sequences become one space. Supported multibyte characters are copied
 * atomically, so a truncated destination never contains partial UTF-8.
 * @param in Null-terminated UTF-8 input.
 * @param out Destination UTF-8 buffer.
 * @param out_size Destination capacity in bytes, including the terminating NUL.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int subtitle_text_sanitize(char const* const in, char* const out, size_t const out_size)
{
    size_t input = 0U;
    size_t output = 0U;

    if ((in == NULL) || (out == NULL) || (out_size == 0U))
    {
        return -EINVAL;
    }

    while (in[input] != '\0')
    {
        unsigned char const lead = (unsigned char)in[input];
        uint8_t const length = encoded_length(lead);

        if (lead <= SANITIZE_ASCII_MAX)
        {
            char const replacement = normalize_ascii(lead);
            emit_bytes(out, out_size, &output, &replacement, 1U);
            input++;
        }
        else if ((length == 2U) && (in[input + 1U] != '\0')
                 && (is_continuation((unsigned char)in[input + 1U]) != 0U)
                 && (is_supported_spanish(lead, (unsigned char)in[input + 1U]) != 0U))
        {
            emit_bytes(out, out_size, &output, &in[input], 2U);
            input += 2U;
        }
        else if ((lead == 0xE2U) && ((unsigned char)in[input + 1U] == 0x80U)
                 && (in[input + 2U] != '\0'))
        {
            unsigned char const punctuation = (unsigned char)in[input + 2U];
            char const* replacement = ((punctuation >= 0x98U) && (punctuation <= 0x9DU)) ? "'"
                                      : ((punctuation == 0x93U) || (punctuation == 0x94U)) ? "-"
                                      : (punctuation == 0xA6U) ? "..." : " ";
            emit_bytes(out, out_size, &output, replacement, strlen(replacement));
            input += 3U;
        }
        else
        {
            uint8_t consumed = 1U;
            while ((consumed < length) && (in[input + consumed] != '\0')
                   && (is_continuation((unsigned char)in[input + consumed]) != 0U))
            {
                consumed++;
            }
            emit_bytes(out, out_size, &output, " ", 1U);
            input += consumed;
        }
    }

    out[output] = '\0';
    return 0;
}

// === End of documentation ======================================================================================== //
