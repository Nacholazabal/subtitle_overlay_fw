// ====================================================================================================================
// Copyright (c) 2024, <Your Name> <your_email@mail.com>
//
// Some fancy copyright message here (if needed)
// ====================================================================================================================

///
/// @file timestamp.h
/// @brief Helper functions to process timestamps (implementation)
///
// === Headers files inclusions ==================================================================================== //

#include <stdio.h>
#include "stm32f0xx_hal.h"

#include "timestamp.h"

// === Macros definitions ========================================================================================== //

#define TIMESTAMP_STRING_LENGTH (15)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
static char timestamp_string[TIMESTAMP_STRING_LENGTH + 1] = {0};

// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

const char* get_timestamp_hh_mm_ss_ms()
{
    uint32_t ms = get_timestamp_ms();
    uint32_t hours = ms / (1000 * 60 * 60);
    ms %= (1000 * 60 * 60);
    uint32_t minutes = ms / (1000 * 60);
    ms %= (1000 * 60);
    uint32_t seconds = ms / 1000;
    uint32_t remaining_ms = ms % 1000;

    snprintf(timestamp_string, TIMESTAMP_STRING_LENGTH, "%02lu:%02lu:%02lu.%03lu", hours, minutes, seconds,
             remaining_ms);

    return timestamp_string;
}

uint32_t get_timestamp_ms() { return HAL_GetTick(); }

// === End of documentation ======================================================================================== //
