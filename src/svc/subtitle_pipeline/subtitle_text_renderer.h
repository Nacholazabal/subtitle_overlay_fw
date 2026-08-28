/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file subtitle_text_renderer.h
/// @brief Proportional UTF-8 text renderer for subtitle masks
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

/**
 * @brief Renders a subtitle caption and visually distinguishes partial text.
 *
 * Final captions use solid glyphs. For partial captions, only the current
 * segment is dithered; any preceding final segment remains solid. Input is
 * normalized internally to printable ASCII and the supported Spanish
 * characters; unsupported characters are rendered as spaces.
 *
 * The returned width is aligned to 32 pixels (word-aligned) to enable
 * word-at-a-time BRAM writes. The stride is calculated as (width + 7) / 8.
 *
 * @param[in] text Null-terminated UTF-8 caption.
 * @param[in] current_is_final Nonzero when the current segment is final.
 * @param[out] dst Destination mask buffer.
 * @param[in] dst_capacity Destination capacity in bytes.
 * @param[out] width Rendered mask width in pixels (32-pixel aligned).
 * @param[out] height Rendered mask height in pixels.
 * @param[out] stride Bitmap stride in bytes (width / 8, since width is 32-aligned).
 * @return Zero on success, or a negative errno value on failure.
 */
int subtitle_text_renderer_render_caption(char const* text,
                                          uint8_t current_is_final,
                                          uint8_t* dst,
                                          size_t dst_capacity,
                                          uint32_t* width,
                                          uint32_t* height,
                                          uint32_t* stride);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
