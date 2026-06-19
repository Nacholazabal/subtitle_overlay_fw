/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file number_parse.c
/// @brief Checked numeric parsing helper implementation
///

// === Headers files inclusions ==================================================================================== //

#include "number_parse.h"

#include <errno.h>

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

/**
 * @brief Parse an exact span of decimal digits with explicit bounds.
 * @param text Digit span.
 * @param text_len Digit-span length.
 * @param min_value Smallest accepted value.
 * @param max_value Largest accepted value.
 * @param value Parsed value destination.
 * @return 0 on success, -EINVAL for malformed input, or -ERANGE on overflow/range failure.
 */
int number_parse_u32(char const* const text,
                     size_t text_len,
                     uint32_t min_value,
                     uint32_t max_value,
                     uint32_t* const value)
{
    uint32_t parsed = 0U;
    size_t i;

    if ((text == NULL) || (text_len == 0U) || (value == NULL) || (min_value > max_value))
    {
        return -EINVAL;
    }

    for (i = 0U; i < text_len; i++)
    {
        uint32_t digit;

        if ((text[i] < '0') || (text[i] > '9'))
        {
            return -EINVAL;
        }

        digit = (uint32_t)(text[i] - '0');
        if ((digit > max_value) || (parsed > ((max_value - digit) / 10U)))
        {
            return -ERANGE;
        }

        parsed = (parsed * 10U) + digit;
    }

    if (parsed < min_value)
    {
        return -ERANGE;
    }

    *value = parsed;
    return 0;
}

// === End of documentation ======================================================================================== //
