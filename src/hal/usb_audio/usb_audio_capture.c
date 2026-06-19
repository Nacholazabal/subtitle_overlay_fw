/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file usb_audio_capture.c
/// @brief ALSA USB audio capture adapter implementation
///

// === Headers files inclusions ==================================================================================== //

#include "usb_audio_capture.h"

#include <alloca.h>
#include <stdlib.h>
#include <string.h>

#include "errorno.h"
#include "log.h"
#include "number_parse.h"

#ifdef CONFIG_USB_AUDIO_ALSA
    #include <alsa/asoundlib.h>
#endif

// === Macros definitions ========================================================================================== //

#define USB_AUDIO_CAPTURE_SAMPLE_BYTES   (2U)
#define USB_AUDIO_CAPTURE_BUFFER_PERIODS (8U)
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

#ifdef CONFIG_USB_AUDIO_ALSA
static int configure_pcm(snd_pcm_t* pcm, usb_audio_capture_config_t const* config);
static int recover_pcm(snd_pcm_t* pcm, int err);
static char const* pcm_state_name(snd_pcm_state_t state);
static void set_capture_gain(char const* device);
#endif

#define USB_AUDIO_CAPTURE_DEFAULT_VOL_PCT (100L)

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

#ifdef CONFIG_USB_AUDIO_ALSA
/**
 * @brief Configure ALSA hardware parameters for STT-ready PCM capture.
 * @param pcm Open ALSA PCM handle.
 * @param config Requested capture configuration.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int configure_pcm(snd_pcm_t* const pcm, usb_audio_capture_config_t const* const config)
{
    snd_pcm_hw_params_t* params;
    snd_pcm_uframes_t period_frames = config->samples_per_chunk;
    snd_pcm_uframes_t buffer_frames = config->samples_per_chunk * USB_AUDIO_CAPTURE_BUFFER_PERIODS;
    unsigned int rate = config->sample_rate_hz;
    int status;

    snd_pcm_hw_params_alloca(&params);

    status = snd_pcm_hw_params_any(pcm, params);
    if (status < 0)
    {
        return -EIO;
    }

    status = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (status < 0)
    {
        return -EIO;
    }

    status = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    if (status < 0)
    {
        return -EIO;
    }

    status = snd_pcm_hw_params_set_channels(pcm, params, config->channels);
    if (status < 0)
    {
        return -EIO;
    }

    status = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, NULL);
    if ((status < 0) || (rate != config->sample_rate_hz))
    {
        return -EIO;
    }

    status = snd_pcm_hw_params_set_period_size_near(pcm, params, &period_frames, NULL);
    if (status < 0)
    {
        return -EIO;
    }

    status = snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_frames);
    if (status < 0)
    {
        return -EIO;
    }

    status = snd_pcm_hw_params(pcm, params);
    if (status < 0)
    {
        LOG_ERROR("usb-audio: ALSA hw params apply failed: %s", snd_strerror(status));
        return -EIO;
    }

    status = snd_pcm_prepare(pcm);
    if (status < 0)
    {
        LOG_ERROR("usb-audio: ALSA prepare failed: %s", snd_strerror(status));
        return -EIO;
    }

    LOG_INFO("usb-audio: ALSA configured rate=%lu channels=%lu period=%lu buffer=%lu",
             (unsigned long)rate,
             (unsigned long)config->channels,
             (unsigned long)period_frames,
             (unsigned long)buffer_frames);
    return 0;
}

/**
 * @brief Recover ALSA capture from transient overrun/suspend states.
 * @param pcm Open ALSA PCM handle.
 * @param err ALSA error code returned by a read operation.
 * @return 0 when recovered, or a negative errno-style value on failure.
 */
static int recover_pcm(snd_pcm_t* const pcm, int err)
{
    int status;

    if (err == -EPIPE)
    {
        status = snd_pcm_prepare(pcm);
        return (status < 0) ? -EIO : 0;
    }

    if (err == -ESTRPIPE)
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

        return (status < 0) ? -EIO : 0;
    }

    return -EIO;
}

/**
 * @brief Return a stable name for an ALSA PCM state.
 * @param state ALSA PCM state value.
 * @return Printable state name.
 */
static char const* pcm_state_name(snd_pcm_state_t const state)
{
    switch (state)
    {
    case SND_PCM_STATE_OPEN:
        return "open";
    case SND_PCM_STATE_SETUP:
        return "setup";
    case SND_PCM_STATE_PREPARED:
        return "prepared";
    case SND_PCM_STATE_RUNNING:
        return "running";
    case SND_PCM_STATE_XRUN:
        return "xrun";
    case SND_PCM_STATE_DRAINING:
        return "draining";
    case SND_PCM_STATE_PAUSED:
        return "paused";
    case SND_PCM_STATE_SUSPENDED:
        return "suspended";
    case SND_PCM_STATE_DISCONNECTED:
        return "disconnected";
    default:
        return "unknown";
    }
}

/**
 * @brief Raise the analog capture gain of the USB soundcard via the ALSA mixer.
 *
 * Sets the capture volume control toward its maximum so the ADC uses more of its
 * range (better SNR) before any digital gain. Best-effort: a missing control or
 * mixer only logs a warning. The control name, card, and percent are overridable
 * via SUBTITLE_USB_AUDIO_CAPTURE_CONTROL / _MIXER / _CAPTURE_VOL_PCT.
 * @param device ALSA PCM device string (used to derive the mixer card).
 * @return None.
 */
static void set_capture_gain(char const* const device)
{
    char const* const control = getenv("SUBTITLE_USB_AUDIO_CAPTURE_CONTROL");
    char const* const mixer_env = getenv("SUBTITLE_USB_AUDIO_MIXER");
    char const* const pct_env = getenv("SUBTITLE_USB_AUDIO_CAPTURE_VOL_PCT");
    char const* const control_name = ((control != NULL) && (control[0] != '\0')) ? control : "Mic";
    char card[16];
    snd_mixer_t* mixer = NULL;
    snd_mixer_selem_id_t* sid;
    snd_mixer_elem_t* elem;
    long pct = USB_AUDIO_CAPTURE_DEFAULT_VOL_PCT;
    long vmin = 0;
    long vmax = 0;
    long value;

    if ((pct_env != NULL) && (pct_env[0] != '\0'))
    {
        uint32_t parsed_pct;

        if (number_parse_u32(pct_env, strlen(pct_env), 0U, 100U, &parsed_pct) == 0)
        {
            pct = (long)parsed_pct;
        }
        else
        {
            LOG_WARNING("usb-audio: ignoring invalid SUBTITLE_USB_AUDIO_CAPTURE_VOL_PCT='%s'",
                        pct_env);
        }
    }

    if ((mixer_env != NULL) && (mixer_env[0] != '\0'))
    {
        snprintf(card, sizeof(card), "%s", mixer_env);
    }
    else
    {
        char const* digit = device;
        while ((*digit != '\0') && ((*digit < '0') || (*digit > '9')))
        {
            digit++;
        }
        snprintf(card, sizeof(card), "hw:%d", (*digit != '\0') ? atoi(digit) : 0);
    }

    if ((snd_mixer_open(&mixer, 0) < 0) || (snd_mixer_attach(mixer, card) < 0)
        || (snd_mixer_selem_register(mixer, NULL, NULL) < 0) || (snd_mixer_load(mixer) < 0))
    {
        LOG_WARNING("usb-audio: could not open mixer on %s to set capture gain", card);
        if (mixer != NULL)
        {
            snd_mixer_close(mixer);
        }
        return;
    }

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, control_name);
    elem = snd_mixer_find_selem(mixer, sid);

    if ((elem == NULL) || (snd_mixer_selem_has_capture_volume(elem) == 0))
    {
        LOG_WARNING("usb-audio: capture control '%s' not found on %s", control_name, card);
        snd_mixer_close(mixer);
        return;
    }

    snd_mixer_selem_get_capture_volume_range(elem, &vmin, &vmax);
    value = vmin + (((vmax - vmin) * pct) / 100L);
    (void)snd_mixer_selem_set_capture_volume_all(elem, value);
    (void)snd_mixer_selem_set_capture_switch_all(elem, 1);
    LOG_INFO("usb-audio: capture gain '%s' on %s set to %ld/%ld (%ld%%)",
             control_name,
             card,
             value,
             vmax,
             pct);

    snd_mixer_close(mixer);
}

#endif

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize USB audio capture through ALSA.
 * @param capture Capture adapter instance.
 * @param config Requested capture configuration.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int usb_audio_capture_init(usb_audio_capture_t* const capture,
                           usb_audio_capture_config_t const* const config)
{
    if ((capture == NULL) || (config == NULL) || (config->device[0] == '\0')
        || (config->sample_rate_hz == 0U) || (config->channels == 0U)
        || (config->samples_per_chunk == 0U))
    {
        return -EINVAL;
    }

#ifdef CONFIG_USB_AUDIO_ALSA
    snd_pcm_t* pcm = NULL;
    int status;

    memset(capture, 0, sizeof(*capture));
    capture->config = *config;
    capture->bytes_per_frame = config->channels * USB_AUDIO_CAPTURE_SAMPLE_BYTES;

    status = snd_pcm_open(&pcm, config->device, SND_PCM_STREAM_CAPTURE, 0);
    if (status < 0)
    {
        LOG_ERROR("usb-audio: ALSA open failed device=%s: %s",
                  config->device,
                  snd_strerror(status));
        return -EIO;
    }

    LOG_INFO("usb-audio: ALSA opened capture device=%s", config->device);

    status = configure_pcm(pcm, config);
    if (status != 0)
    {
        snd_pcm_close(pcm);
        return status;
    }

    set_capture_gain(config->device);

    capture->pcm_handle = pcm;
    capture->initialized = 1U;
    LOG_INFO("usb-audio: ALSA capture initialized chunk_frames=%lu chunk_bytes=%lu",
             (unsigned long)config->samples_per_chunk,
             (unsigned long)(config->samples_per_chunk * capture->bytes_per_frame));
    return 0;
#else
    (void)capture;
    (void)config;
    return -EIO;
#endif
}

/**
 * @brief Read exactly one configured PCM chunk from ALSA.
 * @param capture Initialized capture adapter.
 * @param dst Destination buffer.
 * @param dst_size Destination buffer length in bytes.
 * @param bytes_read Written with bytes captured on success.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int usb_audio_capture_read_chunk(usb_audio_capture_t* const capture,
                                 uint8_t* const dst,
                                 size_t dst_size,
                                 size_t* const bytes_read)
{
    if ((capture == NULL) || (dst == NULL) || (bytes_read == NULL) || (capture->initialized == 0U))
    {
        return -EINVAL;
    }

#ifdef CONFIG_USB_AUDIO_ALSA
    snd_pcm_t* const pcm = (snd_pcm_t*)capture->pcm_handle;
    size_t const expected_bytes =
        (size_t)capture->config.samples_per_chunk * capture->bytes_per_frame;
    snd_pcm_sframes_t frames_done = 0;

    if (dst_size < expected_bytes)
    {
        return -EINVAL;
    }

    while ((uint32_t)frames_done < capture->config.samples_per_chunk)
    {
        uint8_t* const write_ptr = &dst[(size_t)frames_done * capture->bytes_per_frame];
        snd_pcm_uframes_t const frames_left = capture->config.samples_per_chunk
                                              - (uint32_t)frames_done;
        snd_pcm_sframes_t got;

        got = snd_pcm_readi(pcm, write_ptr, frames_left);

        if (got > 0)
        {
            frames_done += got;
        }
        else
        {
            LOG_WARNING(
                "usb-audio: ALSA read returned=%ld state=%s err=%s frames_done=%ld frames_left=%ld",
                (long)got,
                pcm_state_name(snd_pcm_state(pcm)),
                snd_strerror((int)got),
                (long)frames_done,
                (long)frames_left);

            if (recover_pcm(pcm, (int)got) != 0)
            {
                return -EIO;
            }
        }
    }

    *bytes_read = expected_bytes;
    return 0;
#else
    (void)capture;
    (void)dst;
    (void)dst_size;
    (void)bytes_read;
    return -EIO;
#endif
}

/**
 * @brief Abort a blocking ALSA read so a service can stop promptly.
 * @param capture Capture adapter instance.
 * @return None.
 */
void usb_audio_capture_abort(usb_audio_capture_t* const capture)
{
#ifdef CONFIG_USB_AUDIO_ALSA
    if ((capture != NULL) && (capture->pcm_handle != NULL))
    {
        snd_pcm_drop((snd_pcm_t*)capture->pcm_handle);
    }
#else
    (void)capture;
#endif
}

/**
 * @brief Release ALSA capture resources.
 * @param capture Capture adapter instance.
 * @return None.
 */
void usb_audio_capture_cleanup(usb_audio_capture_t* const capture)
{
#ifdef CONFIG_USB_AUDIO_ALSA
    if (capture == NULL)
    {
        return;
    }

    if (capture->pcm_handle != NULL)
    {
        snd_pcm_close((snd_pcm_t*)capture->pcm_handle);
    }

    memset(capture, 0, sizeof(*capture));
#else
    (void)capture;
#endif
}

// === End of documentation ======================================================================================== //
