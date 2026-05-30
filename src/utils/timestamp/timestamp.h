// ================================================================================================================= //
// Copyright (c) 2024, <Your Name> <your_email@mail.com>
//
// Some fancy copyright message here (if needed)
// ================================================================================================================= //

#pragma once

///
/// @file timestamp.h
/// @brief Helper functions to process timestamps
///

// === Headers files inclusions ==================================================================================== //

#include "stdint.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //
// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Get a monotonic increasing timestamp.
/// @return timestamp value.
uint32_t get_timestamp_ms();

/// @brief Given the current timestamp, get a formatted string in hh:mm:ss.mmm format.
/// @return String in in hh:mm:ss.mmm format.
const char* get_timestamp_hh_mm_ss_ms();

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
