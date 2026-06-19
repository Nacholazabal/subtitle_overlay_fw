/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file usb_audio_agc.h
/// @brief Adaptive digital gain (AGC) and level metering for captured PCM
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

/// @brief Adaptive gain state and tuning for one capture stream.
typedef struct
{
    float gain;          ///< Current smoothed gain multiplier.
    float target_peak;   ///< Desired output peak in 0..1 full-scale.
    float max_gain;      ///< Upper clamp for gain.
    float min_gain;      ///< Lower clamp for gain (allows attenuation).
    float attack;        ///< Smoothing coefficient when reducing gain (fast).
    float release;       ///< Smoothing coefficient when raising gain (slow).
    float silence_floor; ///< Below this input peak the gain is held (avoid noise pumping).
} usb_audio_agc_t;

/// @brief Per-chunk metering produced by the AGC for diagnostics.
typedef struct
{
    float raw_peak;     ///< Input peak in 0..1 full-scale before gain.
    float applied_gain; ///< Gain multiplier applied to this chunk.
    float out_peak;     ///< Output peak in 0..1 full-scale after gain.
} usb_audio_agc_metrics_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

void usb_audio_agc_init(usb_audio_agc_t* agc);
void usb_audio_agc_process(usb_audio_agc_t* agc,
                           int16_t* samples,
                           size_t count,
                           usb_audio_agc_metrics_t* metrics);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
