/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file usb_audio_passthrough.h
/// @brief Fixed-capacity optical playback queue and stereo-to-mono downmix.
///

#include <stddef.h>
#include <stdint.h>

#include "usb_audio_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_AUDIO_PASSTHROUGH_QUEUE_CAPACITY (4U)

typedef struct
{
    int16_t samples[USB_AUDIO_CAPTURE_SAMPLES];
    uint32_t frames;
} usb_audio_passthrough_chunk_t;

typedef struct
{
    usb_audio_passthrough_chunk_t chunks[USB_AUDIO_PASSTHROUGH_QUEUE_CAPACITY];
    uint32_t read_index;
    uint32_t write_index;
    uint32_t count;
} usb_audio_passthrough_queue_t;

void usb_audio_passthrough_queue_init(usb_audio_passthrough_queue_t* queue);
int usb_audio_passthrough_queue_push(usb_audio_passthrough_queue_t* queue,
                                     int16_t const* samples,
                                     uint32_t frames,
                                     uint8_t* dropped_oldest);
int usb_audio_passthrough_queue_pop(usb_audio_passthrough_queue_t* queue,
                                    usb_audio_passthrough_chunk_t* chunk);
int usb_audio_passthrough_downmix(int16_t const* stereo, int16_t* mono, size_t frames);

#ifdef __cplusplus
}
#endif
