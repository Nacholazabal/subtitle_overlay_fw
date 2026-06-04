/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file video_gpio.h
/// @brief Video GPIO HAL adapter interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "xstatus.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

typedef struct
{
    uintptr_t base;
} video_gpio_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int video_gpio_init(video_gpio_t* gpio);
void video_gpio_set_hpd(video_gpio_t* gpio, uint8_t enabled);
uint8_t video_gpio_is_locked(video_gpio_t const* gpio);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
