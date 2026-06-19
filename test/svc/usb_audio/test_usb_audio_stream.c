#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "errorno.h"
#include "usb_audio_stream.h"

#include "mock_usb_audio_capture.h"

// usb_audio_stream pulls in the real AGC (not mocked); link it explicitly.
TEST_SOURCE_FILE("usb_audio_agc.c")
TEST_SOURCE_FILE("number_parse.c")

static usb_audio_stream_t stream;
static usb_audio_stream_config_t config;

static void init_valid_config(void)
{
    memset(&config, 0, sizeof(config));
    snprintf(config.pcm_device, sizeof(config.pcm_device), "%s", "hw:0,0");
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
    unsetenv("SUBTITLE_USB_AUDIO_AGC_TARGET_PCT");
}

void tearDown(void)
{
    usb_audio_stream_stop(&stream);
    unsetenv("USB_AUDIO_PCM_DEVICE");
    unsetenv("USB_AUDIO_TCP_HOST");
    unsetenv("USB_AUDIO_TCP_PORT");
    unsetenv("SUBTITLE_USB_AUDIO_AGC_TARGET_PCT");
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

void test_usb_audio_stream_default_config_ignores_malformed_or_out_of_range_ports(void)
{
    setenv("USB_AUDIO_TCP_PORT", "70000", 1);
    usb_audio_stream_default_config(&config);
    TEST_ASSERT_EQUAL_UINT32(USB_AUDIO_STREAM_DEFAULT_PORT, config.tcp_port);

    setenv("USB_AUDIO_TCP_PORT", "6000junk", 1);
    usb_audio_stream_default_config(&config);
    TEST_ASSERT_EQUAL_UINT32(USB_AUDIO_STREAM_DEFAULT_PORT, config.tcp_port);
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

    init_valid_config();
    config.tcp_port = 65536U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_stream_start(&stream, &config));
}

void test_usb_audio_stream_invalid_agc_override_retains_default(void)
{
    setenv("SUBTITLE_USB_AUDIO_AGC_TARGET_PCT", "not-a-number", 1);
    usb_audio_capture_init_ExpectAnyArgsAndReturn(-EIO);

    TEST_ASSERT_EQUAL_INT(-EIO, usb_audio_stream_start(&stream, &config));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.45f, stream.agc.target_peak);
}

void test_usb_audio_stream_get_status_reports_lifecycle_and_fatal_error(void)
{
    usb_audio_stream_t status_stream;

    memset(&status_stream, 0, sizeof(status_stream));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, usb_audio_stream_get_status(NULL));
    TEST_ASSERT_EQUAL_INT(-APP_ESTATE, usb_audio_stream_get_status(&status_stream));

    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&status_stream.state_mutex, NULL));
    status_stream.state_initialized = 1U;
    status_stream.running = 1U;
    TEST_ASSERT_EQUAL_INT(0, usb_audio_stream_get_status(&status_stream));

    status_stream.fatal_error = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, usb_audio_stream_get_status(&status_stream));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&status_stream.state_mutex));
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
