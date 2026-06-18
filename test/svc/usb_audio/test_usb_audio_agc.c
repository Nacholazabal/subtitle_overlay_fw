#include <string.h>

#include "unity.h"
#include "usb_audio_agc.h"

#define TEST_CHUNK_SAMPLES (960U)

static usb_audio_agc_t agc;
static int16_t buffer[TEST_CHUNK_SAMPLES];

static void fill_constant(int16_t value)
{
    size_t i;
    for (i = 0U; i < TEST_CHUNK_SAMPLES; i++)
    {
        buffer[i] = value;
    }
}

static usb_audio_agc_metrics_t run_chunks(int16_t amplitude, unsigned iterations)
{
    usb_audio_agc_metrics_t metrics;
    unsigned n;

    memset(&metrics, 0, sizeof(metrics));
    for (n = 0U; n < iterations; n++)
    {
        fill_constant(amplitude);
        usb_audio_agc_process(&agc, buffer, TEST_CHUNK_SAMPLES, &metrics);
    }
    return metrics;
}

void setUp(void)
{
    usb_audio_agc_init(&agc);
}

void tearDown(void)
{}

void test_agc_init_sets_unity_gain_and_defaults(void)
{
    TEST_ASSERT_EQUAL_FLOAT(1.0f, agc.gain);
    TEST_ASSERT_TRUE(agc.target_peak > 0.0f && agc.target_peak < 1.0f);
    TEST_ASSERT_TRUE(agc.max_gain > agc.min_gain);
}

void test_agc_boosts_quiet_input_toward_target_peak(void)
{
    // ~1% full-scale input should be amplified up toward the target peak.
    usb_audio_agc_metrics_t const metrics = run_chunks(320, 600U);

    TEST_ASSERT_TRUE(metrics.applied_gain > 5.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.08f, agc.target_peak, metrics.out_peak);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)320 / 32768.0f, metrics.raw_peak);
}

void test_agc_attenuates_loud_input_and_never_clips(void)
{
    usb_audio_agc_metrics_t const metrics = run_chunks(32000, 50U);

    TEST_ASSERT_TRUE(agc.gain < 1.0f);
    // Output peak stays at/under full scale (no wraparound/overflow).
    TEST_ASSERT_TRUE(metrics.out_peak <= 1.0f);
    TEST_ASSERT_TRUE(metrics.out_peak <= (agc.target_peak + 0.1f));
}

void test_agc_holds_gain_on_silence(void)
{
    usb_audio_agc_metrics_t metrics;

    // First lift the gain on quiet speech...
    (void)run_chunks(320, 200U);
    float const lifted = agc.gain;
    TEST_ASSERT_TRUE(lifted > 1.0f);

    // ...then pure silence must not change the gain or invent output.
    fill_constant(0);
    usb_audio_agc_process(&agc, buffer, TEST_CHUNK_SAMPLES, &metrics);

    TEST_ASSERT_EQUAL_FLOAT(lifted, agc.gain);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, metrics.raw_peak);
    TEST_ASSERT_EQUAL_INT16(0, buffer[0]);
}

void test_agc_tolerates_null_arguments(void)
{
    usb_audio_agc_metrics_t metrics;

    fill_constant(1000);
    usb_audio_agc_process(NULL, buffer, TEST_CHUNK_SAMPLES, &metrics);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, metrics.applied_gain);

    /* Must not crash. */
    usb_audio_agc_process(&agc, NULL, TEST_CHUNK_SAMPLES, &metrics);
    usb_audio_agc_process(&agc, buffer, 0U, &metrics);
}
