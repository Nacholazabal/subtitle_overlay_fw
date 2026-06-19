/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file errorno.h
/// @brief Shared application error numbers
///

// === Headers files inclusions ==================================================================================== //

#include <errno.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

/// @brief Positive application-specific error numbers; return or store failures as negative values.
typedef enum
{
    APP_ESTATE = 1000, ///< Unexpected state for the requested operation.
} app_errorno_e;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //
// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
