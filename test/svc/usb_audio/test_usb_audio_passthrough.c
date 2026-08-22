#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "usb_audio_passthrough.h"

static usb_audio_passthrough_queue_t queue;

void setUp(void)
{
    usb_audio_passthrough_queue_init(&queue);
}

void tearDown(void)
{}

void test_downmix_averages_stereo_without_overflow(void)
{
    int16_t const stereo[] = {
        1000,
        3000,
        32767,
        32767,
        -32768,
        -32768,
        32767,
        -32768,
    };
    int16_t mono[4] = {0};

    TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_downmix(stereo, mono, 4U));
    TEST_ASSERT_EQUAL_INT16(2000, mono[0]);
    TEST_ASSERT_EQUAL_INT16(32767, mono[1]);
    TEST_ASSERT_EQUAL_INT16(-32768, mono[2]);
    TEST_ASSERT_EQUAL_INT16(0, mono[3]);
}

void test_downmix_rejects_invalid_arguments(void)
{
    int16_t samples[2] = {0};

    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_downmix(NULL, samples, 1U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_downmix(samples, NULL, 1U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_downmix(samples, samples, 0U));
}

void test_queue_preserves_fifo_order(void)
{
    int16_t first[USB_AUDIO_CAPTURE_SAMPLES] = {0};
    int16_t second[USB_AUDIO_CAPTURE_SAMPLES] = {0};
    usb_audio_passthrough_chunk_t output;
    uint8_t dropped;

    first[0] = 11;
    second[0] = 22;
    TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_queue_push(&queue, first, 1U, &dropped));
    TEST_ASSERT_EQUAL_UINT8(0U, dropped);
    TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_queue_push(&queue, second, 1U, &dropped));
    TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_queue_pop(&queue, &output));
    TEST_ASSERT_EQUAL_INT16(11, output.samples[0]);
    TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_queue_pop(&queue, &output));
    TEST_ASSERT_EQUAL_INT16(22, output.samples[0]);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, usb_audio_passthrough_queue_pop(&queue, &output));
}

void test_full_queue_drops_oldest_chunk_to_bound_latency(void)
{
    int16_t samples[USB_AUDIO_CAPTURE_SAMPLES] = {0};
    usb_audio_passthrough_chunk_t output;
    uint8_t dropped;
    uint32_t i;

    for (i = 0U; i < USB_AUDIO_PASSTHROUGH_QUEUE_CAPACITY; i++)
    {
        samples[0] = (int16_t)i;
        TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_queue_push(&queue, samples, 1U, &dropped));
        TEST_ASSERT_EQUAL_UINT8(0U, dropped);
    }
    samples[0] = 99;
    TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_queue_push(&queue, samples, 1U, &dropped));
    TEST_ASSERT_EQUAL_UINT8(1U, dropped);
    TEST_ASSERT_EQUAL_INT(0, usb_audio_passthrough_queue_pop(&queue, &output));
    TEST_ASSERT_EQUAL_INT16(1, output.samples[0]);
}

void test_queue_rejects_invalid_arguments(void)
{
    int16_t samples[USB_AUDIO_CAPTURE_SAMPLES] = {0};
    usb_audio_passthrough_chunk_t output;
    uint8_t dropped;

    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_queue_push(NULL, samples, 1U, &dropped));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_queue_push(&queue, NULL, 1U, &dropped));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_queue_push(&queue, samples, 0U, &dropped));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          usb_audio_passthrough_queue_push(&queue,
                                                           samples,
                                                           USB_AUDIO_FRAMES_PER_CHUNK + 1U,
                                                           &dropped));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_queue_pop(NULL, &output));
    TEST_ASSERT_EQUAL_INT(-EINVAL, usb_audio_passthrough_queue_pop(&queue, NULL));
}
