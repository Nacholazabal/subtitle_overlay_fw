/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file errorno.h
/// @brief Shared application error numbers
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

/// @brief Positive application error numbers; return or store failures as negative values.
typedef enum
{
    EIO = 5,     ///< Generic input/output failure.
    EAGAIN = 11, ///< Operation is not complete yet; try again later.
    EINVAL = 22, ///< Invalid argument or unexpected input.
    ESTATE = 32, ///< Unexpected state for the requested operation.
} errorno_e;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //
// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
