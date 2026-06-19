/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file subtitle_text_sanitize.c
/// @brief UTF-8 to approximate-ASCII sanitizer for subtitle text
///

// === Headers files inclusions ==================================================================================== //

#include "subtitle_text_sanitize.h"

#include <stdint.h>

#include "errorno.h"

// === Macros definitions ========================================================================================== //

#define SANITIZE_ASCII_MIN     (0x20U) ///< First printable ASCII byte (space).
#define SANITIZE_ASCII_MAX     (0x7EU) ///< Last printable ASCII byte (tilde).
#define SANITIZE_UTF8_CONT_MIN (0x80U)
#define SANITIZE_UTF8_CONT_MAX (0xBFU)
#define SANITIZE_UTF8_LEAD2    (0xC0U) ///< First two-byte lead byte.
#define SANITIZE_UTF8_LEAD3    (0xE0U) ///< First three-byte lead byte.
#define SANITIZE_UTF8_LEAD4    (0xF0U) ///< First four-byte lead byte.
#define SANITIZE_UTF8_LEAD5    (0xF8U) ///< First invalid lead byte (above four-byte range).

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void emit(char* out, size_t out_size, size_t* idx, char ch);
static uint8_t is_continuation(unsigned char b);
static uint8_t lead_length(unsigned char lead);
static char map_two_byte(unsigned char b2);
static char map_two_byte_c2(unsigned char b2);
static char map_three_byte_punct(unsigned char b3);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Append one character to the output buffer if capacity allows.
 * @param out Destination buffer.
 * @param out_size Destination capacity including the terminating NUL.
 * @param idx Current write index, advanced on success.
 * @param ch Character to append.
 * @return None.
 */
static void emit(char* const out, size_t out_size, size_t* const idx, char ch)
{
    if ((*idx + 1U) < out_size)
    {
        out[(*idx)++] = ch;
    }
}

/**
 * @brief Test whether a byte is a UTF-8 continuation byte.
 * @param b Byte to test.
 * @return 1 when the byte is a continuation byte, 0 otherwise.
 */
static uint8_t is_continuation(unsigned char b)
{
    return ((b >= SANITIZE_UTF8_CONT_MIN) && (b <= SANITIZE_UTF8_CONT_MAX)) ? 1U : 0U;
}

/**
 * @brief Return the encoded length implied by a UTF-8 lead byte.
 * @param lead Candidate lead byte (>= 0x80).
 * @return Sequence length 2, 3, or 4, or 1 for an invalid/stray byte.
 */
static uint8_t lead_length(unsigned char lead)
{
    if ((lead >= SANITIZE_UTF8_LEAD2) && (lead < SANITIZE_UTF8_LEAD3))
    {
        return 2U;
    }

    if ((lead >= SANITIZE_UTF8_LEAD3) && (lead < SANITIZE_UTF8_LEAD4))
    {
        return 3U;
    }

    if ((lead >= SANITIZE_UTF8_LEAD4) && (lead < SANITIZE_UTF8_LEAD5))
    {
        return 4U;
    }

    return 1U;
}

/**
 * @brief Map a Latin-1 supplement character in the 0xC3 block to ASCII.
 * @param b2 Second byte of the 0xC3 sequence.
 * @return Approximate ASCII character, or 0 when unmapped.
 */
static char map_two_byte(unsigned char b2)
{
    switch (b2)
    {
    case 0xA1U: // á
        return 'a';
    case 0xA9U: // é
        return 'e';
    case 0xADU: // í
        return 'i';
    case 0xB3U: // ó
        return 'o';
    case 0xBAU: // ú
    case 0xBCU: // ü
        return 'u';
    case 0xB1U: // ñ
        return 'n';
    case 0x81U: // Á
        return 'A';
    case 0x89U: // É
        return 'E';
    case 0x8DU: // Í
        return 'I';
    case 0x93U: // Ó
        return 'O';
    case 0x9AU: // Ú
    case 0x9CU: // Ü
        return 'U';
    case 0x91U: // Ñ
        return 'N';
    default:
        return '\0';
    }
}

/**
 * @brief Map a character in the 0xC2 block (inverted marks) to ASCII.
 * @param b2 Second byte of the 0xC2 sequence.
 * @return Approximate ASCII character, or 0 when unmapped.
 */
static char map_two_byte_c2(unsigned char b2)
{
    switch (b2)
    {
    case 0xBFU: // ¿
    case 0xA1U: // ¡
        return ' ';
    default:
        return '\0';
    }
}

/**
 * @brief Map common General Punctuation (0xE2 0x80 block) characters to ASCII.
 * @param b3 Third byte of the 0xE2 0x80 sequence.
 * @return Approximate ASCII character, or 0 when unmapped.
 */
static char map_three_byte_punct(unsigned char b3)
{
    switch (b3)
    {
    case 0x9CU: // “
    case 0x9DU: // ”
    case 0x98U: // ‘
    case 0x99U: // ’
        return '\'';
    case 0x93U: // – en dash
    case 0x94U: // — em dash
        return '-';
    case 0xA6U: // … ellipsis
        return '.';
    default:
        return '\0';
    }
}

// === Public function implementation ============================================================================== //

/**
 * @brief Convert UTF-8 subtitle text into approximate printable ASCII.
 *
 * Printable ASCII passes through (with '"' folded to '\''), control bytes become
 * spaces, and recognized Latin/punctuation UTF-8 sequences are folded to ASCII.
 * Any other multibyte sequence is collapsed to a single space so it never renders
 * as several unknown-glyph markers. The output is always NUL-terminated.
 * @param in Null-terminated UTF-8 input.
 * @param out Destination ASCII buffer.
 * @param out_size Destination capacity in bytes (must be at least 1).
 * @return 0 on success, or a negative errno-style value on failure.
 */
int subtitle_text_sanitize(char const* const in, char* const out, size_t out_size)
{
    char const* cursor = in;
    size_t idx = 0U;

    if ((in == NULL) || (out == NULL) || (out_size == 0U))
    {
        return -EINVAL;
    }

    while (*cursor != '\0')
    {
        unsigned char const b = (unsigned char)*cursor;

        if (b <= SANITIZE_ASCII_MAX)
        {
            if (b < SANITIZE_ASCII_MIN)
            {
                emit(out, out_size, &idx, ' ');
            }
            else if (b == (unsigned char)'"')
            {
                emit(out, out_size, &idx, '\'');
            }
            else
            {
                emit(out, out_size, &idx, (char)b);
            }

            cursor++;
            continue;
        }

        uint8_t const length = lead_length(b);
        uint8_t valid = (length >= 2U) ? 1U : 0U;
        uint8_t k;

        // A complete sequence needs (length - 1) continuation bytes. Stop at the
        // terminator so a truncated sequence never reads past it.
        for (k = 1U; (k < length) && (valid != 0U); k++)
        {
            if ((cursor[k] == '\0') || (is_continuation((unsigned char)cursor[k]) == 0U))
            {
                valid = 0U;
            }
        }

        if (valid != 0U)
        {
            unsigned char const b2 = (unsigned char)cursor[1];
            char mapped = '\0';

            if (length == 2U)
            {
                if (b == 0xC3U)
                {
                    mapped = map_two_byte(b2);
                }
                else if (b == 0xC2U)
                {
                    mapped = map_two_byte_c2(b2);
                }
            }
            else if (length == 3U)
            {
                if ((b == 0xE2U) && (b2 == 0x80U))
                {
                    mapped = map_three_byte_punct((unsigned char)cursor[2]);
                }
            }
            else
            {
                // Valid but unmapped (e.g. 4-byte emoji): collapse to one space below.
                mapped = '\0';
            }

            if (mapped != '\0')
            {
                emit(out, out_size, &idx, mapped);
            }
            else
            {
                emit(out, out_size, &idx, ' ');
            }
            cursor += length;
        }
        else
        {
            // Stray continuation, invalid lead, or truncated sequence: emit one space
            // and resync on the next byte.
            emit(out, out_size, &idx, ' ');
            cursor++;
        }
    }

    out[idx] = '\0';
    return 0;
}

// === End of documentation ======================================================================================== //
