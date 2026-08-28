/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file subtitle_pipeline.h
/// @brief Subtitle pipeline service interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "subtitle_bram.h"
#include "subtitle_overlay.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define SUBTITLE_PIPELINE_DEFAULT_BAR_COLOR  0x000000U
#define SUBTITLE_PIPELINE_DEFAULT_TEXT_COLOR 0xFFFFFFU

// === Public data type declarations =============================================================================== //

typedef struct
{
    subtitle_overlay_t overlay;
    subtitle_bram_t bram;
    subtitle_overlay_config_t config;
    uint32_t display_width;
    uint32_t display_height;
    uint8_t platform_ready; ///< holds a reference to the shared MMIO platform.
    uint8_t initialized;
} subtitle_pipeline_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int subtitle_pipeline_init(subtitle_pipeline_t* pipeline,
                           uint32_t display_width,
                           uint32_t display_height);
int subtitle_pipeline_reconfigure(subtitle_pipeline_t* pipeline,
                                  uint32_t display_width,
                                  uint32_t display_height);
void subtitle_pipeline_cleanup(subtitle_pipeline_t* pipeline);
int subtitle_pipeline_clear(subtitle_pipeline_t* pipeline);
int subtitle_pipeline_set_box(subtitle_pipeline_t* pipeline, uint32_t width, uint32_t height);
/// @brief Render a final/partial caption and prepare its compact mask region.
int subtitle_pipeline_write_caption(subtitle_pipeline_t* pipeline,
                                    char const* text,
                                    uint8_t current_is_final);
int subtitle_pipeline_enable(subtitle_pipeline_t* pipeline, uint8_t enabled);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
