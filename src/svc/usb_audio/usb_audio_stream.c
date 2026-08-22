/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file usb_audio_stream.c
/// @brief USB audio capture and nonblocking handoff to the STT subsystem
///

// === Headers files inclusions ==================================================================================== //

#include "usb_audio_stream.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "errorno.h"
#include "log.h"
#include "number_parse.h"
#include "stt_ws_client.h"

// === Macros definitions ========================================================================================== //

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static int now_ns(uint64_t* timestamp_ns);
static void stream_request_stop(usb_audio_stream_t* stream);
static void stream_set_fatal_error(usb_audio_stream_t* stream, int32_t error);
static uint8_t stream_stop_requested(usb_audio_stream_t* stream);
static uint32_t stream_next_sequence(usb_audio_stream_t* stream);
static uint32_t stream_get_total_dropped(usb_audio_stream_t* stream);
static void stream_add_dropped(usb_audio_stream_t* stream, uint32_t dropped);
static int stream_initialize_state(usb_audio_stream_t* stream,
                                   usb_audio_stream_config_t const* config);
static void stream_release_state(usb_audio_stream_t* stream);
static void stream_configure_agc(usb_audio_stream_t* stream);
static void stream_start_passthrough(usb_audio_stream_t* stream);
static void stream_stop_passthrough(usb_audio_stream_t* stream);
static void stream_submit_passthrough(usb_audio_stream_t* stream, int16_t const* samples);
static void stream_log_playback_status(usb_audio_stream_t* stream);
static int capture_stt_chunk(usb_audio_stream_t* stream,
                             usb_audio_stream_chunk_t* chunk,
                             size_t* bytes_read);
static void* capture_thread_main(void* arg);
static void* playback_thread_main(void* arg);
static void copy_env_string(char* dst, size_t dst_size, char const* value);
static uint32_t read_env_u32(char const* name, uint32_t default_value, uint32_t maximum);
static void meter_raw_audio(int16_t const* samples, size_t count, usb_audio_agc_metrics_t* metrics);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Return a monotonic timestamp in nanoseconds.
 * @param timestamp_ns Monotonic timestamp destination.
 * @return 0 on success or -EIO when the platform clock cannot be read.
 */
static int now_ns(uint64_t* const timestamp_ns)
{
    struct timespec ts;

    if ((timestamp_ns == NULL) || (clock_gettime(CLOCK_MONOTONIC, &ts) != 0))
    {
        return -EIO;
    }

    *timestamp_ns = ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
    return 0;
}

static void stream_request_stop(usb_audio_stream_t* const stream)
{
    if ((stream == NULL) || (stream->state_initialized == 0U))
    {
        return;
    }

    pthread_mutex_lock(&stream->state_mutex);
    stream->stop_requested = 1U;
    if (stream->playback_cond_initialized != 0U)
    {
        pthread_cond_broadcast(&stream->playback_cond);
    }
    pthread_mutex_unlock(&stream->state_mutex);
}

/** @brief Preserve the first fatal worker error and request coordinated shutdown. */
static void stream_set_fatal_error(usb_audio_stream_t* const stream, int32_t error)
{
    if ((stream == NULL) || (stream->state_initialized == 0U) || (error >= 0))
    {
        return;
    }

    pthread_mutex_lock(&stream->state_mutex);
    if (stream->fatal_error == 0)
    {
        stream->fatal_error = error;
    }
    stream->stop_requested = 1U;
    pthread_mutex_unlock(&stream->state_mutex);
}

static uint8_t stream_stop_requested(usb_audio_stream_t* const stream)
{
    uint8_t requested = 1U;

    if ((stream == NULL) || (stream->state_initialized == 0U))
    {
        return requested;
    }

    pthread_mutex_lock(&stream->state_mutex);
    requested = stream->stop_requested;
    pthread_mutex_unlock(&stream->state_mutex);
    return requested;
}

static uint32_t stream_next_sequence(usb_audio_stream_t* const stream)
{
    uint32_t sequence;

    pthread_mutex_lock(&stream->state_mutex);
    sequence = stream->next_sequence;
    stream->next_sequence++;
    pthread_mutex_unlock(&stream->state_mutex);
    return sequence;
}

static uint32_t stream_get_total_dropped(usb_audio_stream_t* const stream)
{
    uint32_t dropped;

    pthread_mutex_lock(&stream->state_mutex);
    dropped = stream->total_dropped;
    pthread_mutex_unlock(&stream->state_mutex);
    return dropped;
}

static void stream_add_dropped(usb_audio_stream_t* const stream, uint32_t dropped)
{
    if (dropped == 0U)
    {
        return;
    }

    pthread_mutex_lock(&stream->state_mutex);
    stream->total_dropped += dropped;
    pthread_mutex_unlock(&stream->state_mutex);
}

static int stream_initialize_state(usb_audio_stream_t* const stream,
                                   usb_audio_stream_config_t const* const config)
{
    int status;

    memset(stream, 0, sizeof(*stream));
    stream->config = *config;
    status = pthread_mutex_init(&stream->state_mutex, NULL);
    if (status != 0)
    {
        return -EIO;
    }
    stream->state_initialized = 1U;
    status = pthread_cond_init(&stream->playback_cond, NULL);
    if (status != 0)
    {
        pthread_mutex_destroy(&stream->state_mutex);
        stream->state_initialized = 0U;
        return -EIO;
    }
    stream->playback_cond_initialized = 1U;
    usb_audio_passthrough_queue_init(&stream->passthrough_queue);
    return 0;
}

static void stream_release_state(usb_audio_stream_t* const stream)
{
    if (stream->playback_cond_initialized != 0U)
    {
        pthread_cond_destroy(&stream->playback_cond);
        stream->playback_cond_initialized = 0U;
    }
    if (stream->state_initialized != 0U)
    {
        pthread_mutex_destroy(&stream->state_mutex);
        stream->state_initialized = 0U;
    }
}

static void stream_configure_agc(usb_audio_stream_t* const stream)
{
    char const* const target = getenv("SUBTITLE_USB_AUDIO_AGC_TARGET_PCT");

    usb_audio_agc_init(&stream->agc);
    stream->agc_enabled = (uint8_t)read_env_u32("SUBTITLE_USB_AUDIO_AGC_ENABLE", 0U, 1U);
    if ((target != NULL) && (target[0] != '\0'))
    {
        uint32_t pct;

        if (number_parse_u32(target, strlen(target), 1U, 100U, &pct) == 0)
        {
            stream->agc.target_peak = (float)pct / 100.0f;
        }
        else
        {
            LOG_WARNING("usb-audio: ignoring invalid SUBTITLE_USB_AUDIO_AGC_TARGET_PCT='%s'",
                        target);
        }
    }
    if (stream->agc_enabled == 0U)
    {
        LOG_INFO("usb-audio: digital AGC disabled; streaming raw PCM");
    }
}

static void* playback_thread_main(void* const arg)
{
    usb_audio_stream_t* const stream = (usb_audio_stream_t*)arg;

    LOG_INFO("usb-audio: optical playback thread started");
    for (;;)
    {
        usb_audio_passthrough_chunk_t chunk;
        int status;

        pthread_mutex_lock(&stream->state_mutex);
        while ((stream->passthrough_queue.count == 0U) && (stream->stop_requested == 0U))
        {
            pthread_cond_wait(&stream->playback_cond, &stream->state_mutex);
        }
        if ((stream->passthrough_queue.count == 0U) && (stream->stop_requested != 0U))
        {
            pthread_mutex_unlock(&stream->state_mutex);
            break;
        }
        status = usb_audio_passthrough_queue_pop(&stream->passthrough_queue, &chunk);
        pthread_mutex_unlock(&stream->state_mutex);
        if (status != 0)
        {
            continue;
        }

        status = usb_audio_playback_write_chunk(&stream->playback,
                                                (uint8_t const*)chunk.samples,
                                                (size_t)chunk.frames * USB_AUDIO_PLAYBACK_CHANNELS
                                                    * USB_AUDIO_SAMPLE_BYTES);
        pthread_mutex_lock(&stream->state_mutex);
        if (status == 0)
        {
            stream->playback_chunks_written++;
            stream->playback_recoveries = stream->playback.recoveries;
        }
        else if (stream->stop_requested == 0U)
        {
            stream->playback_error = status;
            stream->playback_active = 0U;
        }
        pthread_mutex_unlock(&stream->state_mutex);
        if (status != 0)
        {
            if (stream_stop_requested(stream) == 0U)
            {
                LOG_WARNING("usb-audio: optical playback disabled after write error=%ld",
                            (long)status);
            }
            break;
        }
    }

    LOG_INFO("usb-audio: optical playback thread stopped");
    return NULL;
}

static void stream_start_passthrough(usb_audio_stream_t* const stream)
{
    usb_audio_playback_config_t config;
    int status;

    if (stream->config.passthrough_enabled == 0U)
    {
        LOG_INFO("usb-audio: optical passthrough disabled by configuration");
        return;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.device, sizeof(config.device), "%s", stream->config.playback_pcm_device);
    config.sample_rate_hz = USB_AUDIO_SAMPLE_RATE_HZ;
    config.channels = USB_AUDIO_PLAYBACK_CHANNELS;
    config.frames_per_chunk = USB_AUDIO_FRAMES_PER_CHUNK;
    config.volume_pct = stream->config.playback_volume_pct;
    status = usb_audio_playback_init(&stream->playback, &config);
    if (status != 0)
    {
        stream->playback_error = status;
        LOG_WARNING("usb-audio: optical passthrough unavailable, STT capture will continue");
        return;
    }

    stream->playback_active = 1U;
    status = pthread_create(&stream->playback_thread, NULL, playback_thread_main, stream);
    if (status != 0)
    {
        stream->playback_active = 0U;
        stream->playback_error = -EIO;
        usb_audio_playback_cleanup(&stream->playback);
        LOG_WARNING("usb-audio: optical playback thread unavailable, STT capture will continue");
        return;
    }
    stream->playback_thread_started = 1U;
}

static void stream_stop_passthrough(usb_audio_stream_t* const stream)
{
    if (stream->playback_thread_started != 0U)
    {
        usb_audio_playback_abort(&stream->playback);
        pthread_join(stream->playback_thread, NULL);
        stream->playback_thread_started = 0U;
    }
    if (stream->playback.initialized != 0U)
    {
        usb_audio_playback_cleanup(&stream->playback);
    }
    stream->playback_active = 0U;
}

static void stream_submit_passthrough(usb_audio_stream_t* const stream,
                                      int16_t const* const samples)
{
    uint8_t dropped = 0U;

    pthread_mutex_lock(&stream->state_mutex);
    if (stream->playback_active != 0U)
    {
        if (usb_audio_passthrough_queue_push(&stream->passthrough_queue,
                                             samples,
                                             USB_AUDIO_FRAMES_PER_CHUNK,
                                             &dropped)
            == 0)
        {
            stream->playback_chunks_dropped += dropped;
            pthread_cond_signal(&stream->playback_cond);
        }
    }
    pthread_mutex_unlock(&stream->state_mutex);
}

static void stream_log_playback_status(usb_audio_stream_t* const stream)
{
    uint32_t written;
    uint32_t dropped;
    uint32_t recoveries;
    int32_t error;
    uint8_t active;

    pthread_mutex_lock(&stream->state_mutex);
    active = stream->playback_active;
    written = stream->playback_chunks_written;
    dropped = stream->playback_chunks_dropped;
    recoveries = stream->playback_recoveries;
    error = stream->playback_error;
    pthread_mutex_unlock(&stream->state_mutex);
    LOG_DEBUG("usb-audio: optical active=%u written=%lu dropped=%lu recoveries=%lu error=%ld",
              (unsigned)active,
              (unsigned long)written,
              (unsigned long)dropped,
              (unsigned long)recoveries,
              (long)error);
}

static int capture_stt_chunk(usb_audio_stream_t* const stream,
                             usb_audio_stream_chunk_t* const chunk,
                             size_t* const bytes_read)
{
    int16_t captured[USB_AUDIO_CAPTURE_SAMPLES];
    size_t captured_bytes = 0U;
    int status;

    status = usb_audio_capture_read_chunk(&stream->capture,
                                          (uint8_t*)captured,
                                          sizeof(captured),
                                          &captured_bytes);
    if (status != 0)
    {
        return status;
    }

    if (stream->playback_thread_started != 0U)
    {
        stream_submit_passthrough(stream, captured);
        status = usb_audio_passthrough_downmix(captured,
                                               (int16_t*)chunk->payload,
                                               USB_AUDIO_FRAMES_PER_CHUNK);
        *bytes_read = (size_t)USB_AUDIO_STT_BYTES_PER_CHUNK;
    }
    else
    {
        memcpy(chunk->payload, captured, (size_t)USB_AUDIO_STT_BYTES_PER_CHUNK);
        *bytes_read = captured_bytes;
    }
    return status;
}

/**
 * @brief Main capture thread: read ALSA chunks and enqueue them.
 * @param arg Stream service instance.
 * @return NULL.
 */
static void* capture_thread_main(void* const arg)
{
    usb_audio_stream_t* const stream = (usb_audio_stream_t*)arg;
    stt_ws_client_t* const client = stt_ws_client_shared();
    uint8_t first_read_pending = 1U;

    LOG_INFO("usb-audio: capture thread started");
    if (client == NULL)
    {
        LOG_ERROR("usb-audio: STT client unavailable; captured PCM will be dropped");
    }

    while (stream_stop_requested(stream) == 0U)
    {
        usb_audio_stream_chunk_t chunk;
        usb_audio_agc_metrics_t metrics;
        size_t bytes_read = 0U;
        int status;

        if (first_read_pending != 0U)
        {
            LOG_INFO("usb-audio: waiting for first ALSA chunk");
        }

        status = capture_stt_chunk(stream, &chunk, &bytes_read);

        if (status != 0)
        {
            if (status == -EAGAIN)
            {
                continue;
            }

            LOG_ERROR("usb-audio: capture read failed, code=%ld", (long)status);
            stream_set_fatal_error(stream, status);
            break;
        }

        if (stream->agc_enabled != 0U)
        {
            // Normalize the captured level on the board and meter it for tuning.
            usb_audio_agc_process(&stream->agc,
                                  (int16_t*)chunk.payload,
                                  bytes_read / USB_AUDIO_SAMPLE_BYTES,
                                  &metrics);
        }
        else
        {
            // Meter only; leave PCM untouched for server-side gain experiments.
            meter_raw_audio((int16_t const*)chunk.payload,
                            bytes_read / USB_AUDIO_SAMPLE_BYTES,
                            &metrics);
        }

        status = now_ns(&chunk.timestamp_ns);
        if (status != 0)
        {
            LOG_ERROR("usb-audio: monotonic clock read failed");
            stream_set_fatal_error(stream, status);
            break;
        }
        chunk.sequence = stream_next_sequence(stream);
        chunk.bytes_used = (uint32_t)bytes_read;
        if ((client == NULL)
            || (stt_ws_client_submit_audio(client,
                                           chunk.payload,
                                           chunk.bytes_used,
                                           chunk.timestamp_ns,
                                           stream_get_total_dropped(stream))
                != 0))
        {
            stream_add_dropped(stream, 1U);
        }

        if ((first_read_pending != 0U) || ((chunk.sequence % 50U) == 0U))
        {
            // Level as % of full scale: in_peak ~1% means a too-quiet capture,
            // ~45% is healthy, >95% risks clipping. gain is the digital AGC factor.
            LOG_INFO("usb-audio: level agc=%s in_peak=%.1f%% gain=%.1fx out_peak=%.1f%%",
                     (stream->agc_enabled != 0U) ? "on" : "off",
                     (double)(metrics.raw_peak * 100.0f),
                     (double)metrics.applied_gain,
                     (double)(metrics.out_peak * 100.0f));
            LOG_DEBUG("usb-audio: submitted chunk seq=%lu bytes=%lu dropped=%lu link=%s",
                      (unsigned long)chunk.sequence,
                      (unsigned long)chunk.bytes_used,
                      (unsigned long)stream_get_total_dropped(stream),
                      (client != NULL) ? stt_ws_client_state_name(stt_ws_client_state(client))
                                       : "unavailable");
            stream_log_playback_status(stream);
            first_read_pending = 0U;
        }
    }

    LOG_INFO("usb-audio: capture thread stopped");
    return NULL;
}

/**
 * @brief Meter PCM without modifying samples.
 * @param samples S16_LE samples.
 * @param count Number of samples.
 * @param metrics Metering output.
 * @return None.
 */
static void meter_raw_audio(int16_t const* const samples,
                            size_t const count,
                            usb_audio_agc_metrics_t* const metrics)
{
    float peak = 0.0f;
    size_t i;

    if (metrics == NULL)
    {
        return;
    }

    if ((samples == NULL) || (count == 0U))
    {
        metrics->raw_peak = 0.0f;
        metrics->applied_gain = 1.0f;
        metrics->out_peak = 0.0f;
        return;
    }

    for (i = 0U; i < count; i++)
    {
        float sample = (float)samples[i] / 32768.0f;

        if (sample < 0.0f)
        {
            sample = -sample;
        }
        if (sample > peak)
        {
            peak = sample;
        }
    }

    metrics->raw_peak = peak;
    metrics->applied_gain = 1.0f;
    metrics->out_peak = peak;
}

/**
 * @brief Copy an environment string into a fixed-size config field.
 * @param dst Destination string.
 * @param dst_size Destination capacity.
 * @param value Optional source string.
 * @return None.
 */
static void copy_env_string(char* const dst, size_t dst_size, char const* const value)
{
    if ((value == NULL) || (value[0] == '\0'))
    {
        return;
    }

    snprintf(dst, dst_size, "%s", value);
}

static uint32_t read_env_u32(char const* const name,
                             uint32_t const default_value,
                             uint32_t const maximum)
{
    char const* const text = getenv(name);
    uint32_t value;

    if ((text == NULL) || (text[0] == '\0'))
    {
        return default_value;
    }
    if (number_parse_u32(text, strlen(text), 0U, maximum, &value) != 0)
    {
        LOG_WARNING("usb-audio: ignoring invalid %s='%s'", name, text);
        return default_value;
    }
    return value;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Fill a USB audio stream configuration from defaults and environment.
 * @param config Configuration to initialize.
 * @return None.
 */
void usb_audio_stream_default_config(usb_audio_stream_config_t* const config)
{
    if (config == NULL)
    {
        return;
    }

    memset(config, 0, sizeof(*config));
    snprintf(config->pcm_device, sizeof(config->pcm_device), "%s", USB_AUDIO_STREAM_DEFAULT_DEVICE);
    copy_env_string(config->pcm_device, sizeof(config->pcm_device), getenv("USB_AUDIO_PCM_DEVICE"));
    snprintf(config->playback_pcm_device,
             sizeof(config->playback_pcm_device),
             "%s",
             config->pcm_device);
    copy_env_string(config->playback_pcm_device,
                    sizeof(config->playback_pcm_device),
                    getenv("USB_AUDIO_PLAYBACK_PCM_DEVICE"));
    config->passthrough_enabled =
        (uint8_t)read_env_u32("SUBTITLE_USB_AUDIO_PASSTHROUGH_ENABLE", 1U, 1U);
    config->playback_volume_pct = read_env_u32("SUBTITLE_USB_AUDIO_PLAYBACK_VOL_PCT",
                                               USB_AUDIO_STREAM_DEFAULT_PLAYBACK_VOLUME_PCT,
                                               100U);
}

/**
 * @brief Start the ALSA capture worker.
 * @param stream Stream service instance.
 * @param config Runtime configuration.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int usb_audio_stream_start(usb_audio_stream_t* const stream,
                           usb_audio_stream_config_t const* const config)
{
    usb_audio_capture_config_t capture_config;
    int status;

    if ((stream == NULL) || (config == NULL) || (config->pcm_device[0] == '\0')
        || (config->playback_volume_pct > 100U)
        || ((config->passthrough_enabled != 0U) && (config->playback_pcm_device[0] == '\0')))
    {
        return -EINVAL;
    }

    status = stream_initialize_state(stream, config);
    if (status != 0)
    {
        return status;
    }
    stream_configure_agc(stream);
    stream_start_passthrough(stream);

    memset(&capture_config, 0, sizeof(capture_config));
    snprintf(capture_config.device, sizeof(capture_config.device), "%s", config->pcm_device);
    capture_config.sample_rate_hz = USB_AUDIO_SAMPLE_RATE_HZ;
    capture_config.channels = (stream->playback_thread_started != 0U) ? USB_AUDIO_CAPTURE_CHANNELS
                                                                      : USB_AUDIO_STT_CHANNELS;
    capture_config.samples_per_chunk = USB_AUDIO_FRAMES_PER_CHUNK;

    status = usb_audio_capture_init(&stream->capture, &capture_config);
    if (status != 0)
    {
        stream_request_stop(stream);
        stream_stop_passthrough(stream);
        stream_release_state(stream);
        return status;
    }

    status = pthread_create(&stream->capture_thread, NULL, capture_thread_main, stream);
    if (status != 0)
    {
        usb_audio_capture_cleanup(&stream->capture);
        stream_request_stop(stream);
        stream_stop_passthrough(stream);
        stream_release_state(stream);
        return -EIO;
    }

    stream->running = 1U;
    return 0;
}

/**
 * @brief Read the current worker health without blocking.
 * @param stream Stream service instance.
 * @return 0 while healthy, a stored fatal worker error, or -APP_ESTATE when not running.
 */
int usb_audio_stream_get_status(usb_audio_stream_t* const stream)
{
    int status;

    if ((stream == NULL) || (stream->state_initialized == 0U) || (stream->running == 0U))
    {
        return -APP_ESTATE;
    }

    pthread_mutex_lock(&stream->state_mutex);
    status = stream->fatal_error;
    pthread_mutex_unlock(&stream->state_mutex);
    return status;
}

/**
 * @brief Stop the capture worker and release ALSA resources.
 * @param stream Stream service instance.
 * @return None.
 */
void usb_audio_stream_stop(usb_audio_stream_t* const stream)
{
    if ((stream == NULL) || (stream->running == 0U))
    {
        return;
    }

    stream_request_stop(stream);
    usb_audio_capture_abort(&stream->capture);

    pthread_join(stream->capture_thread, NULL);
    stream_stop_passthrough(stream);
    usb_audio_capture_cleanup(&stream->capture);
    stream->running = 0U;
    stream_release_state(stream);
}

// === End of documentation ======================================================================================== //
