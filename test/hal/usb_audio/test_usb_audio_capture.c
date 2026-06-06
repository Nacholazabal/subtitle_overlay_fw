#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "errorno.h"
#include "usb_audio_capture.h"

static usb_audio_capture_t capture;
static usb_audio_capture_config_t config;
static uint8_t chunk[640];
static size_t bytes_read;

static void init_valid_config(void)
{
    memset(&config, 0, sizeof(config));
    snprintf(config.device, sizeof(config.device), "%s", "plughw:1,0");
    config.sample_rate_hz = 16000U;
    config.channels = 1U;
    config.samples_per_chunk = 320U;
}

void setUp(void)
{
    memset(&capture, 0, sizeof(capture));
    memset(chunk, 0, sizeof(chunk));
    bytes_read = 0U;
    init_valid_config();
}

void tearDown(void)
{
}

void test_usb_audio_capture_init_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_capture_init(NULL, &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_capture_init(&capture, NULL));

    config.device[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_capture_init(&capture, &config));

    init_valid_config();
    config.sample_rate_hz = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_capture_init(&capture, &config));

    init_valid_config();
    config.channels = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_capture_init(&capture, &config));

    init_valid_config();
    config.samples_per_chunk = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_capture_init(&capture, &config));
}

void test_usb_audio_capture_init_reports_unavailable_when_alsa_is_disabled(void)
{
    TEST_ASSERT_EQUAL_INT(-EIO, usb_audio_capture_init(&capture, &config));
    TEST_ASSERT_EQUAL_UINT8(0U, capture.initialized);
}

void test_usb_audio_capture_read_reports_unavailable_when_alsa_is_disabled(void)
{
    capture.initialized = 1U;

    TEST_ASSERT_EQUAL_INT(-EIO,
                          usb_audio_capture_read_chunk(&capture,
                                                       chunk,
                                                       sizeof(chunk),
                                                       &bytes_read));
    TEST_ASSERT_EQUAL_UINT(0U, bytes_read);
}

void test_usb_audio_capture_abort_and_cleanup_are_safe_for_empty_capture(void)
{
    usb_audio_capture_abort(NULL);
    usb_audio_capture_abort(&capture);
    usb_audio_capture_cleanup(NULL);
    usb_audio_capture_cleanup(&capture);
}
