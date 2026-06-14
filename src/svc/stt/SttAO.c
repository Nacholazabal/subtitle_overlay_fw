/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file SttAO.c
/// @brief Speech-to-text input active object
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "log.h"
#include "SttAO.h"
#include "SubtitleAO.h"
#include "stt_event_rx.h"

// === Macros definitions ========================================================================================== //

#define STT_AO_POLL_TICKS              (1U)
#define STT_AO_POLL_PERIOD_MS          (10U)
#define STT_AO_PARTIAL_EVENT_MARGIN    (4U)
#define STT_AO_FINAL_EVENT_MARGIN      (1U)

// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;
    QTimeEvt poll_time_evt;

    stt_event_rx_t rx;
    uint32_t last_seq;
    uint8_t have_last_seq;
    uint8_t running;
} stt_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState stt_ao_initial(stt_ao_t* const me, void const* const par);
static QState stt_ao_idle(stt_ao_t* const me, QEvt const* const e);
static QState stt_ao_ready(stt_ao_t* const me, QEvt const* const e);
static QState stt_ao_error(stt_ao_t* const me, QEvt const* const e);

static void post_ready(stt_ao_t* const me);
static void post_error(stt_ao_t* const me, int32_t code);
static int on_component_init(stt_ao_t* const me);
static int on_poll(stt_ao_t* const me);
static int on_transcript(stt_ao_t* const me, subtitle_text_evt_t const* const e);
static void enter_error(stt_ao_t* const me, int32_t code);

// === Private variable definitions ================================================================================ //

static stt_ao_t stt_ao_inst;

// === Public variable definitions ================================================================================= //

QActive* const AO_Stt = Q_ACTIVE_UPCAST(&stt_ao_inst);

// === Private function implementation ============================================================================= //

/**
 * @brief Post a component-ready report to system_ao_t.
 * @param me STT active object sending the report.
 * @return None.
 */
static void post_ready(stt_ao_t* const me)
{
    component_ready_evt_t* const ready_evt =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_READY_SIG);

    Q_UNUSED_PAR(me);

    if (ready_evt == NULL)
    {
        LOG_ERROR("stt: failed to allocate ready event");
        return;
    }

    ready_evt->source = COMPONENT_STT;
    ready_evt->width = 0U;
    ready_evt->height = 0U;
    if (!QACTIVE_POST_X(AO_System, &ready_evt->super, APP_CONTROL_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("stt: failed to post ready event");
    }
}

/**
 * @brief Post a component-error report to system_ao_t.
 * @param me STT active object sending the report.
 * @param code Negative errorno_e value to include in the report.
 * @return None.
 */
static void post_error(stt_ao_t* const me, int32_t code)
{
    app_error_evt_t* const error_evt =
        Q_NEW_X(app_error_evt_t, APP_ERROR_EVENT_MARGIN, COMPONENT_ERROR_SIG);

    Q_UNUSED_PAR(me);

    if (error_evt == NULL)
    {
        LOG_ERROR("stt: failed to allocate error event, code=%ld", (long)code);
        return;
    }

    error_evt->source = COMPONENT_STT;
    error_evt->code = code;
    if (!QACTIVE_POST_X(AO_System, &error_evt->super, APP_ERROR_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("stt: failed to post error event, code=%ld", (long)code);
    }
}

/**
 * @brief Start the STT transcript TCP receiver.
 * @param me STT active object.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int on_component_init(stt_ao_t* const me)
{
    stt_event_rx_config_t config;
    int status;

    stt_event_rx_default_config(&config);

    LOG_INFO("stt: starting transcript receiver on %s:%lu",
             config.host,
             (unsigned long)config.port);

    status = stt_event_rx_init(&me->rx, &config);
    if (status == 0)
    {
        me->running = 1U;
        QTimeEvt_armX(&me->poll_time_evt, STT_AO_POLL_TICKS, STT_AO_POLL_TICKS);
        post_ready(me);
        LOG_INFO("stt: transcript receiver ready, poll period=%lu ms",
                 (unsigned long)STT_AO_POLL_PERIOD_MS);
    }
    else
    {
        LOG_ERROR("stt: receiver initialization failed, code=%ld", (long)status);
        enter_error(me, status);
    }

    return status;
}

/**
 * @brief Poll the STT receiver and forward parsed transcript events.
 * @param me STT active object.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int on_poll(stt_ao_t* const me)
{
    subtitle_text_evt_t events[STT_EVENT_RX_MAX_EVENTS_PER_POLL];
    uint32_t event_count = 0U;
    uint32_t i;
    int status;

    status = stt_event_rx_poll(&me->rx, events, Q_DIM(events), &event_count);
    if (status != 0)
    {
        LOG_ERROR("stt: receiver poll failed, code=%ld", (long)status);
        return status;
    }

    for (i = 0U; i < event_count; i++)
    {
        (void)on_transcript(me, &events[i]);
    }

    return 0;
}

/**
 * @brief Forward a valid transcript event to the subtitle active object.
 * @param me STT active object.
 * @param e Transcript event.
 * @return 0 on success, -EAGAIN when discarded.
 */
static int on_transcript(stt_ao_t* const me, subtitle_text_evt_t const* const e)
{
    subtitle_text_evt_t* subtitle_evt;
    uint16_t const margin =
        (e->is_final != 0U) ? STT_AO_FINAL_EVENT_MARGIN : STT_AO_PARTIAL_EVENT_MARGIN;

    if ((e == NULL) || (e->text[0] == '\0'))
    {
        return -EINVAL;
    }

    if ((me->have_last_seq != 0U) && (e->seq <= me->last_seq))
    {
        LOG_WARNING("stt: discarding old transcript seq=%lu last=%lu",
                    (unsigned long)e->seq,
                    (unsigned long)me->last_seq);
        return -EAGAIN;
    }

    me->have_last_seq = 1U;
    me->last_seq = e->seq;

    subtitle_evt = Q_NEW_X(subtitle_text_evt_t, margin, SUBTITLE_TEXT_SIG);
    if (subtitle_evt == NULL)
    {
        LOG_WARNING("stt: dropping %s transcript seq=%lu, event pool margin=%u",
                    (e->is_final != 0U) ? "final" : "partial",
                    (unsigned long)e->seq,
                    (unsigned)margin);
        return -EAGAIN;
    }

    subtitle_evt->seq = e->seq;
    subtitle_evt->start_ms = e->start_ms;
    subtitle_evt->end_ms = e->end_ms;
    subtitle_evt->is_final = e->is_final;
    snprintf(subtitle_evt->text, sizeof(subtitle_evt->text), "%s", e->text);

    LOG_INFO("stt: forwarding %s transcript seq=%lu",
             (e->is_final != 0U) ? "final" : "partial",
             (unsigned long)e->seq);
    if (!QACTIVE_POST_X(AO_Subtitle, &subtitle_evt->super, margin, &me->super))
    {
        LOG_WARNING("stt: dropping %s transcript seq=%lu, subtitle queue margin=%u",
                    (e->is_final != 0U) ? "final" : "partial",
                    (unsigned long)e->seq,
                    (unsigned)margin);
        return -EAGAIN;
    }

    return 0;
}

/**
 * @brief Stop the STT receiver and report a terminal error.
 * @param me STT active object entering error.
 * @param code Negative errorno_e value.
 * @return None.
 */
static void enter_error(stt_ao_t* const me, int32_t code)
{
    if (me->running != 0U)
    {
        (void)QTimeEvt_disarm(&me->poll_time_evt);
        stt_event_rx_cleanup(&me->rx);
        me->running = 0U;
    }

    post_error(me, code);
}

/**
 * @brief Run the initial transition for stt_ao_t.
 * @param me STT active object instance.
 * @param par Optional initial transition parameter.
 * @return QP/C transition result.
 */
static QState stt_ao_initial(stt_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&stt_ao_idle);
}

/**
 * @brief Handle initialization before the STT receiver is running.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_idle(stt_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_INIT_SIG:
        if (on_component_init(me) == 0)
        {
            status = Q_TRAN(&stt_ao_ready);
        }
        else
        {
            status = Q_TRAN(&stt_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Hold the running STT receiver and forward transcript events.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_ready(stt_ao_t* const me, QEvt const* const e)
{
    QState status;
    int poll_status;

    switch (e->sig)
    {
    case STT_POLL_SIG:
        poll_status = on_poll(me);
        if (poll_status == 0)
        {
            status = Q_HANDLED();
        }
        else
        {
            enter_error(me, poll_status);
            status = Q_TRAN(&stt_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state reached after STT receiver initialization fails.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_error(stt_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    return Q_SUPER(&QHsm_top);
}

// === Public function implementation ============================================================================== //

/**
 * @brief Construct the STT active object.
 * @param None.
 * @return None.
 */
void stt_ao_ctor(void)
{
    stt_ao_t* const me = &stt_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&stt_ao_initial));
    QTimeEvt_ctorX(&me->poll_time_evt, &me->super, STT_POLL_SIG, 0U);
    me->last_seq = 0U;
    me->have_last_seq = 0U;
    me->running = 0U;
}

// === End of documentation ======================================================================================== //
