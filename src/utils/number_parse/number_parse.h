/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file number_parse.h
/// @brief Checked numeric parsing helpers
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //
// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Parse an exact span of ASCII decimal digits into a bounded uint32_t.
 * @param text Digit span; it does not need to be NUL-terminated.
 * @param text_len Number of bytes in @p text.
 * @param min_value Smallest accepted value.
 * @param max_value Largest accepted value.
 * @param value Parsed value destination, unchanged on failure.
 * @return 0 on success, -EINVAL for malformed input, or -ERANGE outside the requested range.
 */
int number_parse_u32(char const* text,
                     size_t text_len,
                     uint32_t min_value,
                     uint32_t max_value,
                     uint32_t* value);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
