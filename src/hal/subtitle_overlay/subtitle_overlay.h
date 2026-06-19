/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file subtitle_overlay.h
/// @brief Subtitle overlay AXI-Lite HAL adapter interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define SUBTITLE_OVERLAY_CTRL_ENABLE 0x00000001U
#define SUBTITLE_OVERLAY_CTRL_SOF    0x00000002U

// === Public data type declarations =============================================================================== //

typedef struct
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t bar_color;
    uint32_t text_color;
} subtitle_overlay_config_t;

typedef struct
{
    uintptr_t base;
} subtitle_overlay_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int subtitle_overlay_init(subtitle_overlay_t* overlay);
int subtitle_overlay_configure(subtitle_overlay_t* overlay,
                               subtitle_overlay_config_t const* config);
int subtitle_overlay_enable(subtitle_overlay_t* overlay, int enabled);
int subtitle_overlay_read_control(subtitle_overlay_t const* overlay, uint32_t* control);
int subtitle_overlay_clear_sof(subtitle_overlay_t* overlay);
int subtitle_overlay_wait_sof(subtitle_overlay_t* overlay, uint32_t timeout_reads);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
