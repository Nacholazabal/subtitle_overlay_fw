#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "errorno.h"
#include "usb_audio_stream.h"

#include "mock_usb_audio_capture.h"

static usb_audio_stream_t stream;
static usb_audio_stream_config_t config;

static void init_valid_config(void)
{
    memset(&config, 0, sizeof(config));
    snprintf(config.pcm_device, sizeof(config.pcm_device), "%s", "plughw:1,0");
    snprintf(config.tcp_host, sizeof(config.tcp_host), "%s", "127.0.0.1");
    config.tcp_port = 5000U;
}

void setUp(void)
{
    memset(&stream, 0, sizeof(stream));
    init_valid_config();
    unsetenv("USB_AUDIO_PCM_DEVICE");
    unsetenv("USB_AUDIO_TCP_HOST");
    unsetenv("USB_AUDIO_TCP_PORT");
}

void tearDown(void)
{
    usb_audio_stream_stop(&stream);
    unsetenv("USB_AUDIO_PCM_DEVICE");
    unsetenv("USB_AUDIO_TCP_HOST");
    unsetenv("USB_AUDIO_TCP_PORT");
}

void test_usb_audio_stream_default_config_uses_expected_defaults(void)
{
    usb_audio_stream_default_config(&config);

    TEST_ASSERT_EQUAL_STRING(USB_AUDIO_STREAM_DEFAULT_DEVICE, config.pcm_device);
    TEST_ASSERT_EQUAL_STRING(USB_AUDIO_STREAM_DEFAULT_HOST, config.tcp_host);
    TEST_ASSERT_EQUAL_UINT32(USB_AUDIO_STREAM_DEFAULT_PORT, config.tcp_port);
}

void test_usb_audio_stream_default_config_reads_environment_overrides(void)
{
    setenv("USB_AUDIO_PCM_DEVICE", "hw:2,0", 1);
    setenv("USB_AUDIO_TCP_HOST", "10.0.0.50", 1);
    setenv("USB_AUDIO_TCP_PORT", "6000", 1);

    usb_audio_stream_default_config(&config);

    TEST_ASSERT_EQUAL_STRING("hw:2,0", config.pcm_device);
    TEST_ASSERT_EQUAL_STRING("10.0.0.50", config.tcp_host);
    TEST_ASSERT_EQUAL_UINT32(6000U, config.tcp_port);
}

void test_usb_audio_stream_start_rejects_invalid_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_stream_start(NULL, &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_stream_start(&stream, NULL));

    config.pcm_device[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_stream_start(&stream, &config));

    init_valid_config();
    config.tcp_host[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_stream_start(&stream, &config));

    init_valid_config();
    config.tcp_port = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_stream_start(&stream, &config));
}

void test_usb_audio_stream_start_reports_capture_unavailable_when_alsa_is_disabled(void)
{
    usb_audio_capture_init_ExpectAnyArgsAndReturn(-EIO);

    TEST_ASSERT_EQUAL_INT(-EIO, usb_audio_stream_start(&stream, &config));
    TEST_ASSERT_EQUAL_UINT8(0U, stream.running);
}

void test_usb_audio_stream_stop_ignores_null_or_not_running_stream(void)
{
    usb_audio_stream_stop(NULL);
    usb_audio_stream_stop(&stream);
    TEST_ASSERT_EQUAL_UINT8(0U, stream.running);
}
