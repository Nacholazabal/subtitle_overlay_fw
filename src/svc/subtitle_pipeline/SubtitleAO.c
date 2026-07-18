/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file SubtitleAO.c
/// @brief Subtitle pipeline orchestration active object
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "log.h"
#include "number_parse.h"
#include "SubtitleAO.h"
#include "subtitle_pipeline.h"

// === Macros definitions ========================================================================================== //

#define SUBTITLE_AO_DONE_WIDTH         (32)
#define SUBTITLE_AO_DONE_HEIGHT        (8)
#define SUBTITLE_AO_DONE_X             ((SUBTITLE_BRAM_MASK_WIDTH - SUBTITLE_AO_DONE_WIDTH) / 2)
#define SUBTITLE_AO_DONE_Y             ((SUBTITLE_BRAM_MASK_HEIGHT - SUBTITLE_AO_DONE_HEIGHT) / 2)
#define SUBTITLE_AO_DONE_BYTES_PER_ROW (SUBTITLE_AO_DONE_WIDTH / 8)

/// Broadcast captions: keep one previous final segment briefly above the live segment.
#define SUBTITLE_AO_SLOT_MAX   (SUBTITLE_TEXT_MAX_LEN)
#define SUBTITLE_AO_RENDER_MAX ((SUBTITLE_AO_SLOT_MAX * 2U) + 2U)

/// Inactivity timeout: clear the overlay and reset broadcast slots when no new subtitle
/// text arrives for this long, so stale captions disappear instead of lingering.
#define SUBTITLE_AO_TICKS_PER_SEC            (100U)
#define SUBTITLE_AO_CLEAR_TIMEOUT_MS         (5000U)
#define SUBTITLE_AO_CLEAR_TIMEOUT_MIN_MS     (1000U)
#define SUBTITLE_AO_PREVIOUS_HOLD_MS         (3000U)
#define SUBTITLE_AO_PREVIOUS_HOLD_MIN_MS     (250U)

// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;
    QTimeEvt clear_time_evt;
    QTimeEvt previous_expire_evt;

    subtitle_pipeline_t pipeline;
    char previous_final[SUBTITLE_AO_SLOT_MAX];
    char current_text[SUBTITLE_AO_SLOT_MAX];
    uint32_t clear_timeout_ticks;
    uint32_t previous_hold_ticks;
    uint8_t previous_visible;
    uint8_t current_valid;
    uint8_t current_is_final;
    uint8_t running;
} subtitle_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState subtitle_ao_initial(subtitle_ao_t* const me, void const* const par);
static QState subtitle_ao_idle(subtitle_ao_t* const me, QEvt const* const e);
static QState subtitle_ao_ready(subtitle_ao_t* const me, QEvt const* const e);
static QState subtitle_ao_error(subtitle_ao_t* const me, QEvt const* const e);

static void post_ready(subtitle_ao_t* const me);
static void post_error(subtitle_ao_t* const me, int32_t code);
static int on_component_init(subtitle_ao_t* const me, component_init_evt_t const* const e);
static int on_subtitle_text(subtitle_ao_t* const me, subtitle_text_evt_t const* const e);
static int on_previous_expired(subtitle_ao_t* const me);
static int render_current_state(subtitle_ao_t* const me);
static void promote_current_to_previous(subtitle_ao_t* const me);
static void reset_text_state(subtitle_ao_t* const me);
static void clear_subtitle(subtitle_ao_t* const me);
static uint32_t ms_to_ticks(uint32_t timeout_ms);
static uint32_t resolve_timeout_ticks(char const* env_name, uint32_t default_ms, uint32_t min_ms);
static uint32_t resolve_clear_timeout_ticks(void);
static uint32_t resolve_previous_hold_ticks(void);
static int draw_startup_marker(subtitle_ao_t* const me);
static void enter_error(subtitle_ao_t* const me, int32_t code);

// === Private variable definitions ================================================================================ //

static subtitle_ao_t subtitle_ao_inst;

static uint8_t const done_bitmap[SUBTITLE_AO_DONE_HEIGHT][SUBTITLE_AO_DONE_BYTES_PER_ROW] = {
    {0xF0U, 0x70U, 0x88U, 0xF8U},
    {0x88U, 0x88U, 0xC8U, 0x80U},
    {0x84U, 0x88U, 0xA8U, 0x80U},
    {0x84U, 0x88U, 0x98U, 0xF0U},
    {0x84U, 0x88U, 0x88U, 0x80U},
    {0x88U, 0x88U, 0x88U, 0x80U},
    {0xF0U, 0x70U, 0x88U, 0xF8U},
    {0x00U, 0x00U, 0x00U, 0x00U},
};

// === Public variable definitions ================================================================================= //

QActive* const AO_Subtitle = Q_ACTIVE_UPCAST(&subtitle_ao_inst);

// === Private function implementation ============================================================================= //

/**
 * @brief Post a component-ready report to the system active object.
 * @param me Subtitle active object sending the report.
 * @return None.
 */
static void post_ready(subtitle_ao_t* const me)
{
    component_ready_evt_t* const ready_evt =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_READY_SIG);

    if (ready_evt == NULL)
    {
        LOG_ERROR("subtitle: failed to allocate ready event");
        return;
    }

    ready_evt->source = COMPONENT_SUBTITLE_PIPELINE;
    ready_evt->width = me->pipeline.display_width;
    ready_evt->height = me->pipeline.display_height;
    if (!QACTIVE_POST_X(AO_System, &ready_evt->super, APP_CONTROL_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("subtitle: failed to post ready event");
    }
}

/**
 * @brief Post a component-error report to the system active object.
 * @param me Subtitle active object sending the report.
 * @param code Negative errno-style value to include in the report.
 * @return None.
 */
static void post_error(subtitle_ao_t* const me, int32_t code)
{
    app_error_evt_t* const error_evt =
        Q_NEW_X(app_error_evt_t, APP_ERROR_EVENT_MARGIN, COMPONENT_ERROR_SIG);

    Q_UNUSED_PAR(me);

    if (error_evt == NULL)
    {
        LOG_ERROR("subtitle: failed to allocate error event, code=%ld", (long)code);
        return;
    }

    error_evt->source = COMPONENT_SUBTITLE_PIPELINE;
    error_evt->code = code;
    if (!QACTIVE_POST_X(AO_System, &error_evt->super, APP_ERROR_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("subtitle: failed to post error event, code=%ld", (long)code);
    }
}

/**
 * @brief Draw the temporary startup marker into subtitle BRAM.
 * @param me Subtitle active object owning the pipeline.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int draw_startup_marker(subtitle_ao_t* const me)
{
    int status;

    LOG_INFO("subtitle: drawing startup marker");

    status = subtitle_pipeline_clear(&me->pipeline);
    if (status != 0)
    {
        LOG_ERROR("subtitle: clear failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_write_bitmap(&me->pipeline,
                                            &done_bitmap[0][0],
                                            sizeof(done_bitmap),
                                            SUBTITLE_AO_DONE_X,
                                            SUBTITLE_AO_DONE_Y,
                                            SUBTITLE_AO_DONE_WIDTH,
                                            SUBTITLE_AO_DONE_HEIGHT);
    if (status != 0)
    {
        LOG_ERROR("subtitle: bitmap write failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_enable(&me->pipeline, 1U);
    if (status != 0)
    {
        LOG_ERROR("subtitle: enable failed, code=%ld", (long)status);
        return status;
    }

    return 0;
}

/**
 * @brief Initialize the subtitle pipeline and display a temporary DONE marker.
 * @param me Subtitle active object receiving COMPONENT_INIT_SIG.
 * @param e Initialization event carrying the active video dimensions.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int on_component_init(subtitle_ao_t* const me, component_init_evt_t const* const e)
{
    int status;

    if ((e == NULL) || (e->width == 0U) || (e->height == 0U))
    {
        status = -EINVAL;
    }
    else
    {
        LOG_INFO("subtitle: initializing pipeline for %lux%lu",
                 (unsigned long)e->width,
                 (unsigned long)e->height);

        status = subtitle_pipeline_init(&me->pipeline, e->width, e->height);
        if (status == 0)
        {
            status = draw_startup_marker(me);
        }
    }

    if (status == 0)
    {
        me->running = 1U;
        // SRC-M02: the startup DONE marker is a temporary diagnostic. Arm the
        // inactivity clear timer now so it is removed after the normal timeout
        // even if no STT transcript ever arrives.
        QTimeEvt_rearm(&me->clear_time_evt, me->clear_timeout_ticks);
        post_ready(me);
        LOG_INFO("subtitle: pipeline ready");
    }
    else
    {
        LOG_ERROR("subtitle: initialization failed, code=%ld", (long)status);
        enter_error(me, status);
    }

    return status;
}

static void reset_text_state(subtitle_ao_t* const me)
{
    me->previous_final[0] = '\0';
    me->current_text[0] = '\0';
    me->previous_visible = 0U;
    me->current_valid = 0U;
    me->current_is_final = 0U;
}

static void promote_current_to_previous(subtitle_ao_t* const me)
{
    if ((me->current_valid != 0U) && (me->current_text[0] != '\0'))
    {
        snprintf(me->previous_final, sizeof(me->previous_final), "%s", me->current_text);
        me->previous_visible = 1U;
        QTimeEvt_rearm(&me->previous_expire_evt, me->previous_hold_ticks);
    }
}

/**
 * @brief Blank the overlay and reset the visible broadcast caption slots.
 * @param me Subtitle active object owning the pipeline.
 * @return None.
 */
static void clear_subtitle(subtitle_ao_t* const me)
{
    int status;

    reset_text_state(me);
    (void)QTimeEvt_disarm(&me->previous_expire_evt);

    // SRC-M03: surface (do not silently discard) failures to blank the overlay,
    // since the logical text state is being reset regardless and a failure here
    // means the physical overlay may still be showing stale content.
    status = subtitle_pipeline_clear(&me->pipeline);
    if (status != 0)
    {
        LOG_WARNING("subtitle: clear failed while blanking, code=%ld", (long)status);
    }

    status = subtitle_pipeline_enable(&me->pipeline, 0U);
    if (status != 0)
    {
        LOG_WARNING("subtitle: disable failed while blanking, code=%ld", (long)status);
    }
}

static uint32_t ms_to_ticks(uint32_t const timeout_ms)
{
    uint32_t const whole_seconds = timeout_ms / 1000U;
    uint32_t const remaining_ms = timeout_ms % 1000U;

    return (whole_seconds * SUBTITLE_AO_TICKS_PER_SEC)
           + ((remaining_ms * SUBTITLE_AO_TICKS_PER_SEC) / 1000U);
}

static uint32_t resolve_timeout_ticks(char const* const env_name,
                                      uint32_t const default_ms,
                                      uint32_t const min_ms)
{
    char const* const env = getenv(env_name);
    uint32_t timeout_ms = default_ms;

    if ((env != NULL) && (env[0] != '\0'))
    {
        uint32_t parsed;

        if (number_parse_u32(env, strlen(env), min_ms, UINT32_MAX, &parsed) == 0)
        {
            timeout_ms = parsed;
        }
        else
        {
            LOG_WARNING("subtitle: ignoring invalid %s='%s'", env_name, env);
        }
    }

    return ms_to_ticks(timeout_ms);
}

/**
 * @brief Resolve the inactivity clear timeout in QF ticks from the environment.
 * @param None.
 * @return Timeout in QF ticks (clamped to a sane minimum).
 */
static uint32_t resolve_clear_timeout_ticks(void)
{
    return resolve_timeout_ticks("SUBTITLE_CLEAR_TIMEOUT_MS",
                                 SUBTITLE_AO_CLEAR_TIMEOUT_MS,
                                 SUBTITLE_AO_CLEAR_TIMEOUT_MIN_MS);
}

static uint32_t resolve_previous_hold_ticks(void)
{
    return resolve_timeout_ticks("SUBTITLE_PREVIOUS_HOLD_MS",
                                 SUBTITLE_AO_PREVIOUS_HOLD_MS,
                                 SUBTITLE_AO_PREVIOUS_HOLD_MIN_MS);
}

static int render_current_state(subtitle_ao_t* const me)
{
    char render_text[SUBTITLE_AO_RENDER_MAX];
    int status;

    if ((me->previous_visible == 0U) && (me->current_valid == 0U))
    {
        status = subtitle_pipeline_clear(&me->pipeline);
        if (status == 0)
        {
            status = subtitle_pipeline_enable(&me->pipeline, 0U);
        }
        return status;
    }

    if ((me->previous_visible != 0U) && (me->current_valid != 0U))
    {
        snprintf(render_text, sizeof(render_text), "%s\n%s", me->previous_final, me->current_text);
    }
    else if (me->previous_visible != 0U)
    {
        snprintf(render_text, sizeof(render_text), "%s", me->previous_final);
    }
    else
    {
        snprintf(render_text, sizeof(render_text), "%s", me->current_text);
    }

    status = subtitle_pipeline_clear(&me->pipeline);
    if (status == 0)
    {
        status = subtitle_pipeline_write_caption(&me->pipeline,
                                                 render_text,
                                                 me->current_is_final);
    }
    if (status == 0)
    {
        status = subtitle_pipeline_enable(&me->pipeline, 1U);
    }

    return status;
}

/**
 * @brief Render one subtitle text event as part of a broadcast caption pair.
 *
 * A short previous-final slot provides context above the live/current segment.
 * @param me Subtitle active object owning the pipeline.
 * @param e Subtitle text event.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int on_subtitle_text(subtitle_ao_t* const me, subtitle_text_evt_t const* const e)
{
    int status;

    if ((e == NULL) || (e->text[0] == '\0'))
    {
        return -EINVAL;
    }

    if ((me->current_valid != 0U) && (me->current_is_final != 0U))
    {
        promote_current_to_previous(me);
    }

    snprintf(me->current_text, sizeof(me->current_text), "%s", e->text);
    me->current_valid = 1U;
    me->current_is_final = (e->is_final != 0U) ? 1U : 0U;

    LOG_INFO("subtitle: rendering %s seq=%lu text=\"%s\"",
             (e->is_final != 0U) ? "final" : "partial",
             (unsigned long)e->seq,
             e->text);

    status = render_current_state(me);
    if (status != 0)
    {
        LOG_ERROR("subtitle: render failed, code=%ld", (long)status);
        return status;
    }

    // Restart the inactivity timer so the caption clears after a pause in speech.
    QTimeEvt_rearm(&me->clear_time_evt, me->clear_timeout_ticks);

    return status;
}

static int on_previous_expired(subtitle_ao_t* const me)
{
    me->previous_final[0] = '\0';
    me->previous_visible = 0U;
    return render_current_state(me);
}

/**
 * @brief Clean up the subtitle pipeline and report a subtitle AO error.
 * @param me Subtitle active object entering its terminal error state.
 * @param code Negative errno-style value to post to system_ao_t.
 * @return None.
 */
static void enter_error(subtitle_ao_t* const me, int32_t code)
{
    LOG_WARNING("subtitle: cleaning up after error code %ld", (long)code);
    (void)QTimeEvt_disarm(&me->clear_time_evt);
    (void)QTimeEvt_disarm(&me->previous_expire_evt);
    subtitle_pipeline_cleanup(&me->pipeline);
    me->running = 0U;

    post_error(me, code);
}

/**
 * @brief Run the initial transition for subtitle_ao_t.
 * @param me Subtitle active object instance.
 * @param par Optional initial transition parameter supplied by QP/C.
 * @return QP/C transition result.
 */
static QState subtitle_ao_initial(subtitle_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&subtitle_ao_idle);
}

/**
 * @brief Handle component initialization before the subtitle pipeline is running.
 * @param me Subtitle active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState subtitle_ao_idle(subtitle_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_INIT_SIG:
        if (on_component_init(me, Q_EVT_CAST(component_init_evt_t)) == 0)
        {
            status = Q_TRAN(&subtitle_ao_ready);
        }
        else
        {
            status = Q_TRAN(&subtitle_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Hold the initialized subtitle pipeline until future subtitle events arrive.
 * @param me Subtitle active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState subtitle_ao_ready(subtitle_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case SUBTITLE_TEXT_SIG:
        if (on_subtitle_text(me, Q_EVT_CAST(subtitle_text_evt_t)) == 0)
        {
            status = Q_HANDLED();
        }
        else
        {
            enter_error(me, -EIO);
            status = Q_TRAN(&subtitle_ao_error);
        }
        break;

    case SUBTITLE_CLEAR_SIG:
        LOG_INFO("subtitle: clearing stale caption after inactivity");
        clear_subtitle(me);
        status = Q_HANDLED();
        break;

    case SUBTITLE_PREVIOUS_EXPIRE_SIG:
        if (on_previous_expired(me) == 0)
        {
            status = Q_HANDLED();
        }
        else
        {
            enter_error(me, -EIO);
            status = Q_TRAN(&subtitle_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state reached after subtitle pipeline initialization fails.
 * @param me Subtitle active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState subtitle_ao_error(subtitle_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    return Q_SUPER(&QHsm_top);
}

// === Public function implementation ============================================================================== //

/**
 * @brief Construct the subtitle active object.
 * @param None.
 * @return None.
 */
void subtitle_ao_ctor(void)
{
    subtitle_ao_t* const me = &subtitle_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&subtitle_ao_initial));
    QTimeEvt_ctorX(&me->clear_time_evt, &me->super, SUBTITLE_CLEAR_SIG, 0U);
    QTimeEvt_ctorX(&me->previous_expire_evt, &me->super, SUBTITLE_PREVIOUS_EXPIRE_SIG, 0U);
    reset_text_state(me);
    me->clear_timeout_ticks = resolve_clear_timeout_ticks();
    me->previous_hold_ticks = resolve_previous_hold_ticks();
    me->running = 0U;
}

// === End of documentation ======================================================================================== //
