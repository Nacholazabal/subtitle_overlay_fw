/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_json.h
/// @brief Cursor-based scanning for the flat JSON objects the STT server sends
///
/// This is **not** a general JSON library. It is the minimum needed to read the
/// streaming server's messages without allocating: every function advances a
/// caller-owned cursor and copies into caller-owned storage.
///
/// Accepted input is deliberately narrow:
///  * strings carry literal UTF-8; only `\" \\ \/ \b \f \n \r \t` are decoded
///    (whitespace escapes collapse to a space) and `\uXXXX` is rejected;
///  * arrays and nested objects can be skipped but never inspected, bounded by
///    ::STT_JSON_MAX_DEPTH so a hostile message cannot exhaust the stack.
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

/// Deepest array/object nesting skipped before the message is rejected.
#define STT_JSON_MAX_DEPTH (4U)
/// Largest numeric token accepted, including its NUL terminator.
#define STT_JSON_TOKEN_MAX (64U)

// === Public data type declarations =============================================================================== //
// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Advance @p cursor past JSON whitespace.
void stt_json_skip_whitespace(char const** cursor);

/**
 * @brief Parse one JSON string into @p dst.
 * @param cursor Cursor positioned on the opening quote; advanced past the string.
 * @param dst Destination, or NULL to parse and discard.
 * @param dst_size Destination capacity including the NUL terminator.
 * @param require_non_empty When non-zero, an empty string is an error.
 * @param truncated Set to 1 when the value did not fit; may be NULL.
 * @return 0 on success, or -EINVAL on malformed or unsupported input.
 */
int stt_json_parse_string(char const** cursor,
                          char* dst,
                          size_t dst_size,
                          uint8_t require_non_empty,
                          uint8_t* truncated);

/**
 * @brief Return and consume the span of one non-string scalar token.
 * @param cursor Cursor positioned on the token; advanced past it.
 * @param start Receives a pointer to the first token byte.
 * @param length Receives the token length in bytes.
 * @return 0 on success, or -EINVAL when the token is empty.
 */
int stt_json_scalar_span(char const** cursor, char const** start, size_t* length);

/// @brief Parse a uint32 scalar; -EINVAL when malformed, -ERANGE when too large.
int stt_json_parse_u32(char const** cursor, uint32_t* value);

/// @brief Parse a boolean scalar, retaining 0/1 compatibility with older senders.
int stt_json_parse_bool(char const** cursor, uint8_t* value);

/// @brief Parse a finite floating-point scalar, consuming the exact token.
int stt_json_parse_double(char const** cursor, double* value);

/**
 * @brief Skip one value of any kind: scalar, string, array or object.
 *
 * Lets the firmware tolerate fields it does not consume, such as the server's
 * `"att_context_size":[56,6]`, without teaching it their shape.
 *
 * @param cursor Cursor positioned on the value; advanced past it.
 * @return 0 on success, or -EINVAL on malformed or too deeply nested input.
 */
int stt_json_skip_value(char const** cursor);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
