/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file USBAudioAO.c
/// @brief USB audio capture and streaming active object
///

// === Headers files inclusions ==================================================================================== //

#include "USBAudioAO.h"

#include "qpc.h"

#include "app.h"
#include "app_config.h"
#include "log.h"
#include "stt_audio_sink.h"
#include "usb_audio_stream.h"

// === External references ========================================================================================= //

extern app_config_t g_app_config;

// === Macros definitions ========================================================================================== //

#define USB_AUDIO_AO_POLL_TICKS     (10U)
#define USB_AUDIO_AO_POLL_PERIOD_MS (100U)
// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;
    QTimeEvt poll_time_evt;

    usb_audio_stream_t stream;
    uint8_t running;
} usb_audio_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState usb_audio_ao_initial(usb_audio_ao_t* const me, void const* const par);
static QState usb_audio_ao_top(usb_audio_ao_t* const me, QEvt const* const e);
static QState usb_audio_ao_idle(usb_audio_ao_t* const me, QEvt const* const e);
static QState usb_audio_ao_ready(usb_audio_ao_t* const me, QEvt const* const e);
static QState usb_audio_ao_error(usb_audio_ao_t* const me, QEvt const* const e);
static QState usb_audio_ao_stopping(usb_audio_ao_t* const me, QEvt const* const e);
static QState usb_audio_ao_stopped(usb_audio_ao_t* const me, QEvt const* const e);

static int on_component_init(usb_audio_ao_t* const me);
static int on_stream_poll(usb_audio_ao_t* const me);
static void post_ready(usb_audio_ao_t* const me);
static void post_error(usb_audio_ao_t* const me, int32_t code);
static void post_stopped(usb_audio_ao_t* const me);
static void begin_stop(usb_audio_ao_t* const me);
static void complete_stop(usb_audio_ao_t* const me);
static QState leave_running(usb_audio_ao_t* const me);
static void enter_error(usb_audio_ao_t* const me, int32_t code);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

static usb_audio_ao_t usb_audio_ao_inst;

// === Public variable definitions ================================================================================= //

QActive* const AO_USBAudio = Q_ACTIVE_UPCAST(&usb_audio_ao_inst);

// === Private function implementation ============================================================================= //

/**
 * @brief Post a USB-audio ready report to system_ao_t.
 * @param me USB audio active object.
 * @return None.
 */
static void post_ready(usb_audio_ao_t* const me)
{
    component_ready_evt_t* const ready_evt =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_READY_SIG);

    Q_UNUSED_PAR(me);

    if (ready_evt == NULL)
    {
        LOG_ERROR("usb-audio: failed to allocate ready event");
        return;
    }

    ready_evt->source = COMPONENT_USB_AUDIO;
    ready_evt->width = 0U;
    ready_evt->height = 0U;
    if (!QACTIVE_POST_X(AO_System, &ready_evt->super, APP_CONTROL_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("usb-audio: failed to post ready event");
    }
}

/**
 * @brief Post a USB-audio error report to system_ao_t.
 * @param me USB audio active object.
 * @param code Negative errno-style value.
 * @return None.
 */
static void post_error(usb_audio_ao_t* const me, int32_t code)
{
    app_error_evt_t* const error_evt =
        Q_NEW_X(app_error_evt_t, APP_ERROR_EVENT_MARGIN, COMPONENT_ERROR_SIG);

    Q_UNUSED_PAR(me);

    if (error_evt == NULL)
    {
        LOG_ERROR("usb-audio: failed to allocate error event, code=%ld", (long)code);
        return;
    }

    error_evt->source = COMPONENT_USB_AUDIO;
    error_evt->code = code;
    if (!QACTIVE_POST_X(AO_System, &error_evt->super, APP_ERROR_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("usb-audio: failed to post error event, code=%ld", (long)code);
    }
}

/**
 * @brief Start USB audio capture.
 * @param me USB audio active object.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int on_component_init(usb_audio_ao_t* const me)
{
    audio_sink_t sink;
    int status;

    sink = stt_audio_sink_create();
    // STT owns the bounded queue and WebSocket worker; USB owns ALSA only.
    LOG_INFO("usb-audio: starting capture device=%s", g_app_config.audio_stream.pcm_device);

    status = usb_audio_stream_start(&me->stream,
                                    &g_app_config.audio_stream,
                                    &sink,
                                    g_app_config.audio_agc_enabled,
                                    g_app_config.audio_agc_target_pct);
    if (status == 0)
    {
        me->running = 1U;
        QTimeEvt_armX(&me->poll_time_evt, USB_AUDIO_AO_POLL_TICKS, USB_AUDIO_AO_POLL_TICKS);
        post_ready(me);
        LOG_INFO("usb-audio: capture ready, health poll=%u ms",
                 (unsigned)USB_AUDIO_AO_POLL_PERIOD_MS);
    }
    else
    {
        LOG_ERROR("usb-audio: initialization failed, code=%ld", (long)status);
        enter_error(me, status);
    }

    return status;
}

/**
 * @brief Poll worker health and convert a fatal background failure into an AO error.
 * @param me USB audio active object.
 * @return 0 while healthy or the negative worker failure.
 */
static int on_stream_poll(usb_audio_ao_t* const me)
{
    int const status = usb_audio_stream_get_status(&me->stream);

    if (status != 0)
    {
        LOG_ERROR("usb-audio: background worker failed, code=%ld", (long)status);
        enter_error(me, status);
    }

    return status;
}

// Request only: a QP/C handler must not join the capture thread. The poll timer
// stays armed so usb_audio_ao_stopping can observe completion.
static void begin_stop(usb_audio_ao_t* const me)
{
    if (me->running != 0U)
    {
        usb_audio_stream_request_stop(&me->stream);
    }
}

// Joins the worker, which stop_complete has already proved is safe.
static void complete_stop(usb_audio_ao_t* const me)
{
    (void)QTimeEvt_disarm(&me->poll_time_evt);
    if (me->running != 0U)
    {
        (void)usb_audio_stream_finish_stop(&me->stream);
        me->running = 0U;
    }
}

/**
 * @brief Leave the running states, finishing now if the worker already exited.
 * @param me USB audio active object.
 * @return Transition to stopped, or to stopping while the worker winds down.
 */
static QState leave_running(usb_audio_ao_t* const me)
{
    QState status;

    begin_stop(me);
    if ((me->running == 0U) || (usb_audio_stream_stop_complete(&me->stream) != 0U))
    {
        complete_stop(me);
        post_stopped(me);
        status = Q_TRAN(&usb_audio_ao_stopped);
    }
    else
    {
        status = Q_TRAN(&usb_audio_ao_stopping);
    }

    return status;
}

// Acknowledge SYSTEM_STOP to system_ao_t once this AO has quiesced.
static void post_stopped(usb_audio_ao_t* const me)
{
    Q_UNUSED_PAR(me); // used only as the QS trace sender (dropped without Q_SPY)

    component_ready_evt_t* const evt =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, SYSTEM_STOPPED_SIG);

    if (evt != NULL)
    {
        evt->source = COMPONENT_USB_AUDIO;
        evt->width = 0U;
        evt->height = 0U;
        (void)QACTIVE_POST_X(AO_System, &evt->super, APP_CONTROL_EVENT_MARGIN, &me->super);
    }
}

// Requests the stop and reports the fault. The join is left to the SYSTEM_STOP
// handler, which SystemAO triggers in response to the error event.
static void enter_error(usb_audio_ao_t* const me, int32_t code)
{
    begin_stop(me);
    post_error(me, code);
}

/**
 * @brief Run the initial transition for usb_audio_ao_t.
 * @param me USB audio active object.
 * @param par Optional initial parameter.
 * @return QP/C transition result.
 */
static QState usb_audio_ao_initial(usb_audio_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&usb_audio_ao_idle);
}

/**
 * @brief Superstate handling the coordinated SYSTEM_STOP command in any substate.
 * @param me USB audio active object.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState usb_audio_ao_top(usb_audio_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case SYSTEM_STOP_SIG:
        status = leave_running(me);
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Handle initialization before USB audio is running.
 * @param me USB audio active object.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState usb_audio_ao_idle(usb_audio_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_INIT_SIG:
        if (on_component_init(me) == 0)
        {
            status = Q_TRAN(&usb_audio_ao_ready);
        }
        else
        {
            status = Q_TRAN(&usb_audio_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&usb_audio_ao_top);
        break;
    }

    return status;
}

/**
 * @brief Hold the running USB audio service.
 * @param me USB audio active object.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState usb_audio_ao_ready(usb_audio_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case USB_AUDIO_POLL_SIG:
        status = (on_stream_poll(me) == 0) ? Q_HANDLED() : Q_TRAN(&usb_audio_ao_error);
        break;

    default:
        status = Q_SUPER(&usb_audio_ao_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state reached after USB audio initialization fails.
 * @param me USB audio active object.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState usb_audio_ao_error(usb_audio_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    return Q_SUPER(&usb_audio_ao_top);
}

/**
 * @brief Wait for the capture worker to stop, reusing the health-poll timer.
 * @param me USB audio active object.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState usb_audio_ao_stopping(usb_audio_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        LOG_INFO("usb-audio: waiting for capture worker shutdown");
        status = Q_HANDLED();
        break;

    case USB_AUDIO_POLL_SIG:
        if (usb_audio_stream_stop_complete(&me->stream) != 0U)
        {
            complete_stop(me);
            post_stopped(me);
            status = Q_TRAN(&usb_audio_ao_stopped);
        }
        else
        {
            status = Q_HANDLED();
        }
        break;

    case SYSTEM_STOP_SIG:
        status = Q_HANDLED(); // Already stopping.
        break;

    default:
        status = Q_SUPER(&usb_audio_ao_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state after a coordinated SYSTEM_STOP: workers are stopped.
 * @param me USB audio active object.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState usb_audio_ao_stopped(usb_audio_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);

    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        LOG_INFO("usb-audio: stopped");
        status = Q_HANDLED();
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Construct the USB audio active object.
 * @param None.
 * @return None.
 */
void usb_audio_ao_ctor(void)
{
    usb_audio_ao_t* const me = &usb_audio_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&usb_audio_ao_initial));
    QTimeEvt_ctorX(&me->poll_time_evt, &me->super, USB_AUDIO_POLL_SIG, 0U);
    me->running = 0U;
}

// === End of documentation ======================================================================================== //
