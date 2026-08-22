/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file usb_audio_passthrough.c
/// @brief Fixed-capacity optical playback queue and stereo-to-mono downmix.
///

#include "usb_audio_passthrough.h"

#include <errno.h>
#include <string.h>

void usb_audio_passthrough_queue_init(usb_audio_passthrough_queue_t* const queue)
{
    if (queue != NULL)
    {
        memset(queue, 0, sizeof(*queue));
    }
}

int usb_audio_passthrough_queue_push(usb_audio_passthrough_queue_t* const queue,
                                     int16_t const* const samples,
                                     uint32_t const frames,
                                     uint8_t* const dropped_oldest)
{
    usb_audio_passthrough_chunk_t* chunk;

    if ((queue == NULL) || (samples == NULL) || (dropped_oldest == NULL) || (frames == 0U)
        || (frames > USB_AUDIO_FRAMES_PER_CHUNK))
    {
        return -EINVAL;
    }

    *dropped_oldest = 0U;
    if (queue->count == USB_AUDIO_PASSTHROUGH_QUEUE_CAPACITY)
    {
        queue->read_index = (queue->read_index + 1U) % USB_AUDIO_PASSTHROUGH_QUEUE_CAPACITY;
        queue->count--;
        *dropped_oldest = 1U;
    }

    chunk = &queue->chunks[queue->write_index];
    memcpy(chunk->samples,
           samples,
           (size_t)frames * USB_AUDIO_PLAYBACK_CHANNELS * USB_AUDIO_SAMPLE_BYTES);
    chunk->frames = frames;
    queue->write_index = (queue->write_index + 1U) % USB_AUDIO_PASSTHROUGH_QUEUE_CAPACITY;
    queue->count++;
    return 0;
}

int usb_audio_passthrough_queue_pop(usb_audio_passthrough_queue_t* const queue,
                                    usb_audio_passthrough_chunk_t* const chunk)
{
    if ((queue == NULL) || (chunk == NULL))
    {
        return -EINVAL;
    }
    if (queue->count == 0U)
    {
        return -EAGAIN;
    }

    *chunk = queue->chunks[queue->read_index];
    queue->read_index = (queue->read_index + 1U) % USB_AUDIO_PASSTHROUGH_QUEUE_CAPACITY;
    queue->count--;
    return 0;
}

int usb_audio_passthrough_downmix(int16_t const* const stereo,
                                  int16_t* const mono,
                                  size_t const frames)
{
    size_t i;

    if ((stereo == NULL) || (mono == NULL) || (frames == 0U))
    {
        return -EINVAL;
    }

    for (i = 0U; i < frames; i++)
    {
        int32_t const left = stereo[i * USB_AUDIO_CAPTURE_CHANNELS];
        int32_t const right = stereo[(i * USB_AUDIO_CAPTURE_CHANNELS) + 1U];

        mono[i] = (int16_t)((left + right) / 2);
    }
    return 0;
}
