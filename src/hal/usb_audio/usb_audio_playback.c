/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file usb_audio_playback.c
/// @brief Linux ALSA USB audio playback adapter used by the optical S/PDIF path.
///

#include "usb_audio_playback.h"

#include <alloca.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#ifdef CONFIG_USB_AUDIO_ALSA
    #include <alsa/asoundlib.h>
#endif

#define USB_AUDIO_PLAYBACK_SAMPLE_BYTES   (2U)
#define USB_AUDIO_PLAYBACK_BUFFER_PERIODS (3U)

#ifdef CONFIG_USB_AUDIO_ALSA
static int configure_pcm(snd_pcm_t* pcm, usb_audio_playback_config_t const* config);
static int configure_start_threshold(snd_pcm_t* pcm, snd_pcm_uframes_t period_frames);
static int recover_pcm(usb_audio_playback_t* playback, int error);
static void set_playback_level(char const* device, uint32_t volume_pct);

static int configure_start_threshold(snd_pcm_t* const pcm, snd_pcm_uframes_t const period_frames)
{
    snd_pcm_sw_params_t* params;

    snd_pcm_sw_params_alloca(&params);
    if ((snd_pcm_sw_params_current(pcm, params) < 0)
        || (snd_pcm_sw_params_set_start_threshold(pcm, params, period_frames) < 0)
        || (snd_pcm_sw_params_set_avail_min(pcm, params, period_frames) < 0)
        || (snd_pcm_sw_params(pcm, params) < 0))
    {
        return -EIO;
    }
    return 0;
}

static int configure_pcm(snd_pcm_t* const pcm, usb_audio_playback_config_t const* const config)
{
    snd_pcm_hw_params_t* params;
    snd_pcm_uframes_t period_frames = config->frames_per_chunk;
    snd_pcm_uframes_t buffer_frames = period_frames * USB_AUDIO_PLAYBACK_BUFFER_PERIODS;
    unsigned int rate = config->sample_rate_hz;
    int status;

    snd_pcm_hw_params_alloca(&params);
    status = snd_pcm_hw_params_any(pcm, params);
    status = (status < 0)
                 ? status
                 : snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    status = (status < 0) ? status
                          : snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    status = (status < 0) ? status : snd_pcm_hw_params_set_channels(pcm, params, config->channels);
    status = (status < 0) ? status : snd_pcm_hw_params_set_rate_near(pcm, params, &rate, NULL);
    status = (status < 0)
                 ? status
                 : snd_pcm_hw_params_set_period_size_near(pcm, params, &period_frames, NULL);
    status = (status < 0) ? status
                          : snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_frames);
    status = (status < 0) ? status : snd_pcm_hw_params(pcm, params);
    if ((status < 0) || (rate != config->sample_rate_hz)
        || (configure_start_threshold(pcm, period_frames) != 0) || (snd_pcm_prepare(pcm) < 0))
    {
        LOG_ERROR("usb-audio: playback ALSA configuration failed");
        return -EIO;
    }

    LOG_INFO("usb-audio: playback configured rate=%lu channels=%lu period=%lu buffer=%lu",
             (unsigned long)rate,
             (unsigned long)config->channels,
             (unsigned long)period_frames,
             (unsigned long)buffer_frames);
    return 0;
}

static int recover_pcm(usb_audio_playback_t* const playback, int const error)
{
    snd_pcm_t* const pcm = (snd_pcm_t*)playback->pcm_handle;
    int status = error;

    if (error == -EPIPE)
    {
        status = snd_pcm_prepare(pcm);
    }
    else if (error == -ESTRPIPE)
    {
        do
        {
            status = snd_pcm_resume(pcm);
        }
        while (status == -EAGAIN);
        if (status < 0)
        {
            status = snd_pcm_prepare(pcm);
        }
    }

    if (status < 0)
    {
        return -EIO;
    }
    playback->recoveries++;
    return 0;
}

static void set_playback_level(char const* const device, uint32_t const volume_pct)
{
    snd_mixer_t* mixer = NULL;
    snd_mixer_selem_id_t* sid;
    snd_mixer_elem_t* elem;
    char card[16];
    char const* digit = device;
    long minimum = 0;
    long maximum = 0;
    long value;

    while ((*digit != '\0') && ((*digit < '0') || (*digit > '9')))
    {
        digit++;
    }
    snprintf(card, sizeof(card), "hw:%d", (*digit != '\0') ? atoi(digit) : 0);
    if ((snd_mixer_open(&mixer, 0) < 0) || (snd_mixer_attach(mixer, card) < 0)
        || (snd_mixer_selem_register(mixer, NULL, NULL) < 0) || (snd_mixer_load(mixer) < 0))
    {
        LOG_WARNING("usb-audio: could not open playback mixer on %s", card);
        if (mixer != NULL)
        {
            snd_mixer_close(mixer);
        }
        return;
    }

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "PCM");
    elem = snd_mixer_find_selem(mixer, sid);
    if ((elem == NULL) || (snd_mixer_selem_has_playback_volume(elem) == 0))
    {
        LOG_WARNING("usb-audio: playback control 'PCM' not found on %s", card);
        snd_mixer_close(mixer);
        return;
    }

    snd_mixer_selem_get_playback_volume_range(elem, &minimum, &maximum);
    value = minimum + (((maximum - minimum) * (long)volume_pct) / 100L);
    (void)snd_mixer_selem_set_playback_volume_all(elem, value);
    (void)snd_mixer_selem_set_playback_switch_all(elem, 1);
    LOG_INFO("usb-audio: playback level on %s set to %lu%%", card, (unsigned long)volume_pct);
    snd_mixer_close(mixer);
}
#endif

int usb_audio_playback_init(usb_audio_playback_t* const playback,
                            usb_audio_playback_config_t const* const config)
{
    if ((playback == NULL) || (config == NULL) || (config->device[0] == '\0')
        || (config->sample_rate_hz == 0U) || (config->channels == 0U)
        || (config->frames_per_chunk == 0U) || (config->volume_pct > 100U))
    {
        return -EINVAL;
    }

#ifdef CONFIG_USB_AUDIO_ALSA
    snd_pcm_t* pcm = NULL;
    int status;

    memset(playback, 0, sizeof(*playback));
    playback->config = *config;
    playback->bytes_per_frame = config->channels * USB_AUDIO_PLAYBACK_SAMPLE_BYTES;
    status = snd_pcm_open(&pcm, config->device, SND_PCM_STREAM_PLAYBACK, 0);
    if (status < 0)
    {
        LOG_WARNING("usb-audio: playback open failed device=%s: %s",
                    config->device,
                    snd_strerror(status));
        return -EIO;
    }
    if (configure_pcm(pcm, config) != 0)
    {
        snd_pcm_close(pcm);
        return -EIO;
    }
    playback->pcm_handle = pcm;
    playback->initialized = 1U;
    set_playback_level(config->device, config->volume_pct);
    return 0;
#else
    (void)playback;
    (void)config;
    return -EIO;
#endif
}

int usb_audio_playback_write_chunk(usb_audio_playback_t* const playback,
                                   uint8_t const* const src,
                                   size_t const src_size)
{
    if ((playback == NULL) || (src == NULL) || (playback->initialized == 0U))
    {
        return -EINVAL;
    }

#ifdef CONFIG_USB_AUDIO_ALSA
    snd_pcm_t* const pcm = (snd_pcm_t*)playback->pcm_handle;
    size_t const expected = (size_t)playback->config.frames_per_chunk * playback->bytes_per_frame;
    snd_pcm_sframes_t frames_done = 0;

    if (src_size != expected)
    {
        return -EINVAL;
    }
    while ((uint32_t)frames_done < playback->config.frames_per_chunk)
    {
        snd_pcm_uframes_t const remaining = playback->config.frames_per_chunk
                                            - (uint32_t)frames_done;
        uint8_t const* const position = &src[(size_t)frames_done * playback->bytes_per_frame];
        snd_pcm_sframes_t const written = snd_pcm_writei(pcm, position, remaining);

        if (written > 0)
        {
            frames_done += written;
        }
        else if ((written == 0) || (recover_pcm(playback, (int)written) != 0))
        {
            return -EIO;
        }
    }
    return 0;
#else
    (void)src_size;
    return -EIO;
#endif
}

void usb_audio_playback_abort(usb_audio_playback_t* const playback)
{
#ifdef CONFIG_USB_AUDIO_ALSA
    if ((playback != NULL) && (playback->pcm_handle != NULL))
    {
        snd_pcm_drop((snd_pcm_t*)playback->pcm_handle);
    }
#else
    (void)playback;
#endif
}

void usb_audio_playback_cleanup(usb_audio_playback_t* const playback)
{
#ifdef CONFIG_USB_AUDIO_ALSA
    if (playback == NULL)
    {
        return;
    }
    if (playback->pcm_handle != NULL)
    {
        snd_pcm_close((snd_pcm_t*)playback->pcm_handle);
    }
    memset(playback, 0, sizeof(*playback));
#else
    (void)playback;
#endif
}
