/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file USBAudioAO.c
/// @brief USB audio capture and streaming active object
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

#include "app.h"
#include "log.h"
#include "USBAudioAO.h"
#include "usb_audio_stream.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;

    usb_audio_stream_t stream;
    uint8_t running;
} usb_audio_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState usb_audio_ao_initial(usb_audio_ao_t* const me, void const* const par);
static QState usb_audio_ao_idle(usb_audio_ao_t* const me, QEvt const* const e);
static QState usb_audio_ao_ready(usb_audio_ao_t* const me, QEvt const* const e);
static QState usb_audio_ao_error(usb_audio_ao_t* const me, QEvt const* const e);

static int on_component_init(usb_audio_ao_t* const me);
static void post_ready(usb_audio_ao_t* const me);
static void post_error(usb_audio_ao_t* const me, int32_t code);
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
 * @param code Negative errorno_e value.
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
 * @brief Start USB audio capture and TCP streaming.
 * @param me USB audio active object.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int on_component_init(usb_audio_ao_t* const me)
{
    usb_audio_stream_config_t config;
    int status;

    usb_audio_stream_default_config(&config);
    LOG_INFO("usb-audio: starting capture device=%s target=%s:%lu",
             config.pcm_device,
             config.tcp_host,
             (unsigned long)config.tcp_port);

    status = usb_audio_stream_start(&me->stream, &config);
    if (status == 0)
    {
        me->running = 1U;
        post_ready(me);
        LOG_INFO("usb-audio: capture and TCP streaming ready");
    }
    else
    {
        LOG_ERROR("usb-audio: initialization failed, code=%ld", (long)status);
        enter_error(me, status);
    }

    return status;
}

/**
 * @brief Stop USB audio service and report an error.
 * @param me USB audio active object.
 * @param code Negative errorno_e value.
 * @return None.
 */
static void enter_error(usb_audio_ao_t* const me, int32_t code)
{
    if (me->running != 0U)
    {
        usb_audio_stream_stop(&me->stream);
        me->running = 0U;
    }

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
        status = Q_SUPER(&QHsm_top);
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
    Q_UNUSED_PAR(me);

    switch (e->sig)
    {
    default:
        return Q_SUPER(&QHsm_top);
    }
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

    return Q_SUPER(&QHsm_top);
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
    me->running = 0U;
}

// === End of documentation ======================================================================================== //
