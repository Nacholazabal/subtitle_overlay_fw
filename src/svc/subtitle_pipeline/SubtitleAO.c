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

/// Rolling transcript: keep the most recent words across finals so complete
/// phrases stay visible instead of being wiped every time a new event arrives.
#define SUBTITLE_AO_HISTORY_WORDS (48U)
#define SUBTITLE_AO_HISTORY_MAX   (512U)

/// Inactivity timeout: clear the overlay and reset history when no new subtitle
/// text arrives for this long, so stale captions disappear instead of lingering.
#define SUBTITLE_AO_TICKS_PER_SEC        (100U)
#define SUBTITLE_AO_CLEAR_TIMEOUT_MS     (5000U)
#define SUBTITLE_AO_CLEAR_TIMEOUT_MIN_MS (5000U)

// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;
    QTimeEvt clear_time_evt;

    subtitle_pipeline_t pipeline;
    char history[SUBTITLE_AO_HISTORY_MAX];
    uint32_t clear_timeout_ticks;
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
static char const* last_n_words(char const* text, uint32_t n);
static void copy_last_words(char* dst, size_t dst_size, char const* src, uint32_t n);
static void clear_subtitle(subtitle_ao_t* const me);
static uint32_t resolve_clear_timeout_ticks(void);
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

/**
 * @brief Return the start of the last @p n whitespace-separated words in @p text.
 * @param text Null-terminated text.
 * @param n Number of trailing words to keep.
 * @return Pointer into @p text at the start of the last @p n words.
 */
static char const* last_n_words(char const* const text, uint32_t n)
{
    size_t const len = strlen(text);
    uint32_t count = 0U;
    size_t i = len;

    if ((n == 0U) || (len == 0U))
    {
        return text + len;
    }

    while (i > 0U)
    {
        i--;
        if ((text[i] != ' ') && ((i == 0U) || (text[i - 1U] == ' ')))
        {
            count++;
            if (count == n)
            {
                return text + i;
            }
        }
    }

    return text;
}

/**
 * @brief Copy the last @p n words of @p src into @p dst, dropping older words.
 * @param dst Destination buffer.
 * @param dst_size Destination capacity.
 * @param src Source text.
 * @param n Number of trailing words to keep.
 * @return None.
 */
static void copy_last_words(char* const dst, size_t dst_size, char const* const src, uint32_t n)
{
    snprintf(dst, dst_size, "%s", last_n_words(src, n));
}

/**
 * @brief Blank the overlay and reset the rolling transcript.
 * @param me Subtitle active object owning the pipeline.
 * @return None.
 */
static void clear_subtitle(subtitle_ao_t* const me)
{
    me->history[0] = '\0';
    (void)subtitle_pipeline_clear(&me->pipeline);
    (void)subtitle_pipeline_enable(&me->pipeline, 0U);
}

/**
 * @brief Resolve the inactivity clear timeout in QF ticks from the environment.
 * @param None.
 * @return Timeout in QF ticks (clamped to a sane minimum).
 */
static uint32_t resolve_clear_timeout_ticks(void)
{
    char const* const env = getenv("SUBTITLE_CLEAR_TIMEOUT_MS");
    uint32_t timeout_ms = SUBTITLE_AO_CLEAR_TIMEOUT_MS;
    uint32_t whole_seconds;
    uint32_t remaining_ms;

    if ((env != NULL) && (env[0] != '\0'))
    {
        uint32_t parsed;

        if (number_parse_u32(env,
                             strlen(env),
                             SUBTITLE_AO_CLEAR_TIMEOUT_MIN_MS,
                             UINT32_MAX,
                             &parsed)
            == 0)
        {
            timeout_ms = parsed;
        }
        else
        {
            LOG_WARNING("subtitle: ignoring invalid SUBTITLE_CLEAR_TIMEOUT_MS='%s'", env);
        }
    }

    whole_seconds = timeout_ms / 1000U;
    remaining_ms = timeout_ms % 1000U;
    return (whole_seconds * SUBTITLE_AO_TICKS_PER_SEC)
           + ((remaining_ms * SUBTITLE_AO_TICKS_PER_SEC) / 1000U);
}

/**
 * @brief Render one subtitle text event as part of a rolling transcript.
 *
 * Finals are appended to a trimmed history so coherent phrases accumulate, while
 * partials are shown appended to the history without being committed. Only the
 * most recent words are rendered so the newest speech stays visible.
 * @param me Subtitle active object owning the pipeline.
 * @param e Subtitle text event.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int on_subtitle_text(subtitle_ao_t* const me, subtitle_text_evt_t const* const e)
{
    char combined[SUBTITLE_AO_HISTORY_MAX + SUBTITLE_TEXT_MAX_LEN];
    char const* render_src;
    int status;

    if ((e == NULL) || (e->text[0] == '\0'))
    {
        return -EINVAL;
    }

    if (me->history[0] != '\0')
    {
        snprintf(combined, sizeof(combined), "%s %s", me->history, e->text);
    }
    else
    {
        snprintf(combined, sizeof(combined), "%s", e->text);
    }

    if (e->is_final != 0U)
    {
        // Commit the newest words into the rolling history.
        copy_last_words(me->history, sizeof(me->history), combined, SUBTITLE_AO_HISTORY_WORDS);
        render_src = me->history;
    }
    else
    {
        // Show the in-progress partial without committing it to history.
        render_src = last_n_words(combined, SUBTITLE_AO_HISTORY_WORDS);
    }

    LOG_INFO("subtitle: rendering %s seq=%lu text=\"%s\"",
             (e->is_final != 0U) ? "final" : "partial",
             (unsigned long)e->seq,
             e->text);

    status = subtitle_pipeline_clear(&me->pipeline);
    if (status != 0)
    {
        LOG_ERROR("subtitle: clear failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_write_text(&me->pipeline, render_src);
    if (status != 0)
    {
        LOG_ERROR("subtitle: text render failed, code=%ld", (long)status);
        return status;
    }

    status = subtitle_pipeline_enable(&me->pipeline, 1U);
    if (status != 0)
    {
        LOG_ERROR("subtitle: enable failed, code=%ld", (long)status);
        return status;
    }

    // Restart the inactivity timer so the caption clears after a pause in speech.
    QTimeEvt_rearm(&me->clear_time_evt, me->clear_timeout_ticks);

    return status;
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
    me->history[0] = '\0';
    me->clear_timeout_ticks = resolve_clear_timeout_ticks();
    me->running = 0U;
}

// === End of documentation ======================================================================================== //
