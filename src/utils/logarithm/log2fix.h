/**********************************************************************************************************************
Copyright (c) 2024, <Your Name> <your_email@mail.com>

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file log2fix.h
/// @brief Header file for fixed-point logarithm functions.
///
/// This file contains the declarations for functions that compute logarithms in fixed-point arithmetic. The supported
/// logarithms are base 2, natural (base e), and base 10.
///

// === Headers files inclusions ==================================================================================== //
#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //
#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// Add macro definitions here if needed

// === Public data type declarations =============================================================================== //
// Add data type declarations here if needed

// === Public variable declarations ================================================================================ //
// Add variable declarations here if needed

// === Public function declarations ================================================================================ //

/**
/// @brief Computes the base-2 logarithm of a fixed-point number.
///
/// This function calculates the base-2 logarithm of the given fixed-point number with the specified precision.
///
/// @param x The input number for which to calculate the logarithm.
/// @param precision The number of fractional bits in the fixed-point representation.
/// @return The base-2 logarithm of the input number.
 */
int32_t log2fix(uint32_t x, size_t precision);

/// @brief Computes the natural logarithm of a fixed-point number.
///
/// This function calculates the natural logarithm (base e) of the given fixed-point number with the specified
/// precision.
///
/// @param x The input number for which to calculate the logarithm.
/// @param precision The number of fractional bits in the fixed-point representation.
/// @return The natural logarithm of the input number.
int32_t logfix(uint32_t x, size_t precision);

/// @brief Computes the base-10 logarithm of a fixed-point number.
///
/// This function calculates the base-10 logarithm of the given fixed-point number with the specified precision.
///
/// @param x The input number for which to calculate the logarithm.
/// @param precision The number of fractional bits in the fixed-point representation.
/// @return The base-10 logarithm of the input number.
int32_t log10fix(uint32_t x, size_t precision);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
