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
 * @brief Renders UTF-8 subtitle text into a compact 1-bpp mask.
 *
 * Input is normalized internally to printable ASCII and the supported Spanish
 * characters. Unsupported characters are rendered as spaces.
 *
 * @param[in] text Null-terminated UTF-8 text.
 * @param[out] dst Destination mask buffer.
 * @param[in] dst_size Destination capacity in bytes.
 * @param[out] width Rendered mask width in pixels.
 * @param[out] height Rendered mask height in pixels.
 * @return Zero on success, or a negative errno value on failure.
 */
int subtitle_text_renderer_render(char const* text,
                                  uint8_t* dst,
                                  size_t dst_size,
                                  uint32_t* width,
                                  uint32_t* height);

/**
 * @brief Renders a subtitle caption and visually distinguishes partial text.
 *
 * Final captions use solid glyphs. For partial captions, only the current
 * segment is dithered; any preceding final segment remains solid.
 *
 * @param[in] text Null-terminated UTF-8 caption.
 * @param[in] current_is_final Nonzero when the current segment is final.
 * @param[out] dst Destination mask buffer.
 * @param[in] dst_size Destination capacity in bytes.
 * @param[out] width Rendered mask width in pixels.
 * @param[out] height Rendered mask height in pixels.
 * @return Zero on success, or a negative errno value on failure.
 */
int subtitle_text_renderer_render_caption(char const* text,
                                          uint8_t current_is_final,
                                          uint8_t* dst,
                                          size_t dst_size,
                                          uint32_t* width,
                                          uint32_t* height);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
