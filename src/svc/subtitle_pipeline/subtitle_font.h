/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file subtitle_font.h
/// @brief Offline-generated proportional subtitle font interface
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define SUBTITLE_FONT_PIXEL_SIZE (34U)

// === Public data type declarations =============================================================================== //

/// @brief One packed 1-bpp glyph and its horizontal/vertical layout metrics.
typedef struct
{
    uint32_t codepoint; ///< Unicode scalar value represented by this glyph.
    uint16_t width; ///< Bitmap width in pixels.
    uint16_t height; ///< Bitmap height in pixels.
    int16_t x_offset; ///< Horizontal bitmap offset from the pen position.
    int16_t y_offset; ///< Vertical bitmap offset from the baseline.
    uint16_t advance; ///< Horizontal pen advance in pixels.
    uint8_t const* bitmap; ///< Row-major, MSB-first 1-bpp bitmap, or NULL for whitespace.
} subtitle_font_glyph_t;

// === Public function declarations ================================================================================ //

/// @brief Return the baseline-to-baseline distance selected from the font metrics.
uint32_t subtitle_font_line_height(void);

/// @brief Find one Unicode glyph, falling back to the question-mark glyph when unsupported.
subtitle_font_glyph_t const* subtitle_font_lookup(uint32_t codepoint);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
