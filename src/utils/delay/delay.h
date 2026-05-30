/**********************************************************************************************************************
Copyright (c) 2024, <Your Name> <your_email@mail.com>

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file delay.h
/// @brief API for handling non-blocking delays
///

// === Headers files inclusions ==================================================================================== //

#include <stdbool.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

/// @brief Structure representing a non-blocking delay.
typedef struct
{
    uint32_t start_time;  ///< Used to keep tracking of when the delay started
    uint32_t duration;    ///< Duration of the delay, in ms.
    bool running;         ///< Flag to determine whether the delay is still nunning.
} delay_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Initializes a delay with the specified duration. This function DO NOT initializes the delay,
/// in order to do that you'll need to call `delay_has_elapsed()` after initializing the struct.
/// @param delay struct to be initialized
/// @param duration in ms
void delay_init(delay_t* const delay, const uint32_t duration);

/// @brief Queries whether the delay has reached its duration.
/// If the timer is not running, it will start running it first.
/// @param delay struct to be queried.
/// @return `true` if the duration has been reached. `false`, otherwise.
bool delay_has_elapsed(delay_t* const delay);

/// @brief Change the duration of a delay
/// @param delay struct to be written
/// @param duration in ms
void delay_write(delay_t* const delay, const uint32_t duration);

/// @brief Stops a delay. In order to start it again, it's needed to call `delay_has_elapsed()` again.
/// @param delay struct to stop.
void delay_stop(delay_t* const delay);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
