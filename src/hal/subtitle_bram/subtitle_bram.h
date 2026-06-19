/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file subtitle_bram.h
/// @brief Subtitle mask BRAM HAL adapter interface
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define SUBTITLE_BRAM_MASK_WIDTH    1024U
#define SUBTITLE_BRAM_MASK_HEIGHT   256U
#define SUBTITLE_BRAM_WORDS_PER_ROW (SUBTITLE_BRAM_MASK_WIDTH / 32U)
#define SUBTITLE_BRAM_SIZE_BYTES    ((SUBTITLE_BRAM_MASK_WIDTH * SUBTITLE_BRAM_MASK_HEIGHT) / 8U)
#define SUBTITLE_BRAM_WORD_COUNT    (SUBTITLE_BRAM_SIZE_BYTES / sizeof(uint32_t))

// === Public data type declarations =============================================================================== //

typedef struct
{
    uintptr_t base;
} subtitle_bram_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

int subtitle_bram_init(subtitle_bram_t* bram);
int subtitle_bram_clear(subtitle_bram_t* bram);
int subtitle_bram_set_pixel(subtitle_bram_t* bram, int32_t x, int32_t y);
int subtitle_bram_clear_pixel(subtitle_bram_t* bram, int32_t x, int32_t y);
int subtitle_bram_write_bitmap(subtitle_bram_t* bram,
                               uint8_t const* src,
                               size_t src_size,
                               int32_t x,
                               int32_t y,
                               uint32_t width,
                               uint32_t height);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
