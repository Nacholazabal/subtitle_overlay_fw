#include <stdio.h>
#include <string.h>

#include "errorno.h"
#include "unity.h"
#include "usb_audio_playback.h"

static usb_audio_playback_t playback;
static usb_audio_playback_config_t config;
static uint8_t chunk[3840];

static void init_valid_config(void)
{
    memset(&config, 0, sizeof(config));
    snprintf(config.device, sizeof(config.device), "%s", "hw:0,0");
    config.sample_rate_hz = 48000U;
    config.channels = 2U;
    config.frames_per_chunk = 960U;
    config.volume_pct = 100U;
}

void setUp(void)
{
    memset(&playback, 0, sizeof(playback));
    memset(chunk, 0, sizeof(chunk));
    init_valid_config();
}

void tearDown(void)
{}

void test_usb_audio_playback_init_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_init(NULL, &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_init(&playback, NULL));

    config.device[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_init(&playback, &config));
    init_valid_config();
    config.sample_rate_hz = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_init(&playback, &config));
    init_valid_config();
    config.channels = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_init(&playback, &config));
    init_valid_config();
    config.frames_per_chunk = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_init(&playback, &config));
    init_valid_config();
    config.volume_pct = 101U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_init(&playback, &config));
}

void test_usb_audio_playback_reports_unavailable_without_alsa(void)
{
    TEST_ASSERT_EQUAL_INT(-EIO, usb_audio_playback_init(&playback, &config));
    TEST_ASSERT_EQUAL_UINT8(0U, playback.initialized);
}

void test_usb_audio_playback_write_validates_state_and_buffer(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_write_chunk(NULL, chunk, sizeof(chunk)));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_write_chunk(&playback, NULL, sizeof(chunk)));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_playback_write_chunk(&playback, chunk, sizeof(chunk)));

    playback.initialized = 1U;
    playback.config = config;
    playback.bytes_per_frame = 4U;
    TEST_ASSERT_EQUAL_INT(-EIO, usb_audio_playback_write_chunk(&playback, chunk, sizeof(chunk)));
}

void test_usb_audio_playback_abort_and_cleanup_are_safe_for_empty_state(void)
{
    usb_audio_playback_abort(NULL);
    usb_audio_playback_abort(&playback);
    usb_audio_playback_cleanup(NULL);
    usb_audio_playback_cleanup(&playback);
}
