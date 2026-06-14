/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file subtitle_text_renderer.h
/// @brief Minimal text-to-bitmap renderer for subtitle masks
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //
// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int subtitle_text_renderer_render(char const* text,
                                  uint8_t* dst,
                                  size_t dst_size,
                                  uint32_t* width,
                                  uint32_t* height);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
