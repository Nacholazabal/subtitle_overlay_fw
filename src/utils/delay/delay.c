/**********************************************************************************************************************
Copyright (c) 2024, <Your Name> <your_email@mail.com>

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file delay.c
/// @brief API for handling non-blocking delays (implementation)
///

// === Headers files inclusions ==================================================================================== //

#include <assert.h>
#include "stm32f0xx_hal.h"

#include "delay.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

void delay_init(delay_t* const delay, const uint32_t duration)
{
    assert(delay);

    delay->start_time = HAL_GetTick();
    delay->duration = duration;
    delay->running = false;
}

bool delay_has_elapsed(delay_t* const delay)
{
    assert(delay);

    const uint32_t CURRENT_TIME = HAL_GetTick();
    bool ret = false;

    if (!delay->running) {
        delay->start_time = CURRENT_TIME;
        delay->running = true;
    } else {
        ret = (CURRENT_TIME - delay->start_time) >= delay->duration;
        delay->running = !ret;  // stop running flag if we reached the duration.
    }

    return ret;
}

void delay_write(delay_t* const delay, const uint32_t duration)
{
    assert(delay);

    delay->duration = duration;
}

void delay_stop(delay_t* const delay)
{
    assert(delay);

    delay->running = false;
}

// === End of documentation ======================================================================================== //
