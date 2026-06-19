/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file usb_audio_agc.c
/// @brief Adaptive digital gain (AGC) and level metering for captured PCM
///

// === Headers files inclusions ==================================================================================== //

#include "usb_audio_agc.h"

// === Macros definitions ========================================================================================== //

#define AGC_FULL_SCALE      (32768.0f)
#define AGC_SAMPLE_MAX      (32767.0f)
#define AGC_SAMPLE_MIN      (-32768.0f)
#define AGC_DEFAULT_TARGET  (0.45f)
#define AGC_DEFAULT_MAX     (64.0f)
#define AGC_DEFAULT_MIN     (0.25f)
#define AGC_DEFAULT_ATTACK  (0.5f)
#define AGC_DEFAULT_RELEASE (0.05f)
#define AGC_DEFAULT_FLOOR   (0.003f)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static float clampf(float value, float low, float high);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Clamp a float to an inclusive range.
 * @param value Input value.
 * @param low Lower bound.
 * @param high Upper bound.
 * @return Clamped value.
 */
static float clampf(float value, float low, float high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize AGC state with default tuning.
 * @param agc AGC instance to initialize.
 * @return None.
 */
void usb_audio_agc_init(usb_audio_agc_t* const agc)
{
    if (agc == NULL)
    {
        return;
    }

    agc->gain = 1.0f;
    agc->target_peak = AGC_DEFAULT_TARGET;
    agc->max_gain = AGC_DEFAULT_MAX;
    agc->min_gain = AGC_DEFAULT_MIN;
    agc->attack = AGC_DEFAULT_ATTACK;
    agc->release = AGC_DEFAULT_RELEASE;
    agc->silence_floor = AGC_DEFAULT_FLOOR;
}

/**
 * @brief Apply adaptive gain to one PCM chunk in place and report metering.
 *
 * The gain tracks a target output peak: it drops quickly when the input gets
 * louder (avoiding clipping) and rises slowly when it gets quieter, so the level
 * follows changing source volume. Near-silence holds the gain to avoid amplifying
 * background noise.
 * @param agc AGC instance.
 * @param samples Interleaved S16 samples, modified in place.
 * @param count Number of samples.
 * @param metrics Optional metering output for this chunk.
 * @return None.
 */
void usb_audio_agc_process(usb_audio_agc_t* const agc,
                           int16_t* const samples,
                           size_t count,
                           usb_audio_agc_metrics_t* const metrics)
{
    float peak = 0.0f;
    float out_peak = 0.0f;
    size_t i;

    if ((agc == NULL) || (samples == NULL) || (count == 0U))
    {
        if (metrics != NULL)
        {
            metrics->raw_peak = 0.0f;
            metrics->applied_gain = (agc != NULL) ? agc->gain : 1.0f;
            metrics->out_peak = 0.0f;
        }
        return;
    }

    for (i = 0U; i < count; i++)
    {
        float const sample = (float)samples[i] / AGC_FULL_SCALE;
        float const magnitude = (sample < 0.0f) ? -sample : sample;

        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }

    // Update the smoothed gain toward the level that would hit the target peak.
    if (peak > agc->silence_floor)
    {
        float const desired = clampf(agc->target_peak / peak, agc->min_gain, agc->max_gain);
        float const coeff = (desired < agc->gain) ? agc->attack : agc->release;

        agc->gain += coeff * (desired - agc->gain);
    }
    agc->gain = clampf(agc->gain, agc->min_gain, agc->max_gain);

    for (i = 0U; i < count; i++)
    {
        float const scaled = clampf((float)samples[i] * agc->gain, AGC_SAMPLE_MIN, AGC_SAMPLE_MAX);
        float const magnitude = (scaled < 0.0f) ? -scaled : scaled;

        samples[i] = (int16_t)scaled;
        if (magnitude > out_peak)
        {
            out_peak = magnitude;
        }
    }

    if (metrics != NULL)
    {
        metrics->raw_peak = peak;
        metrics->applied_gain = agc->gain;
        metrics->out_peak = out_peak / AGC_FULL_SCALE;
    }
}

// === End of documentation ======================================================================================== //
