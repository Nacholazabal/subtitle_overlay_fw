/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file VideoAO.c
/// @brief Video pipeline orchestration active object
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

#include "app.h"
#include "video_pipeline.h"
#include "VideoAO.h"

// === Macros definitions ========================================================================================== //

#define VIDEO_AO_POLL_TICKS     (10U)
#define VIDEO_AO_POLL_PERIOD_MS (100U)

// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;
    QTimeEvt poll_time_evt;

    video_pipeline_t pipeline;
    uint32_t now_ms;
    int running;
} video_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState video_ao_initial(video_ao_t* const me, void const* const par);
static QState video_ao_idle(video_ao_t* const me, QEvt const* const e);
static QState video_ao_running(video_ao_t* const me, QEvt const* const e);
static QState video_ao_error(video_ao_t* const me, QEvt const* const e);

static void post_ready(video_ao_t* const me);
static void post_error(video_ao_t* const me, int32_t code);
static int on_component_init(video_ao_t* const me);
static int on_video_poll(video_ao_t* const me);
static void enter_error(video_ao_t* const me, int32_t code);

// === Private variable definitions ================================================================================ //

static video_ao_t video_ao_inst;

// === Public variable definitions ================================================================================= //

QActive* const AO_Video = Q_ACTIVE_UPCAST(&video_ao_inst);

// === Private function implementation ============================================================================= //

/**
 * @brief Post a component-ready report to the system active object.
 * @param me Video active object sending the report.
 * @return None.
 */
static void post_ready(video_ao_t* const me)
{
    component_ready_evt_t* const ready_evt = Q_NEW(component_ready_evt_t, COMPONENT_READY_SIG);

    Q_UNUSED_PAR(me);

    ready_evt->source = COMPONENT_VIDEO;
    QACTIVE_POST(AO_System, &ready_evt->super, &me->super);
}

/**
 * @brief Post a component-error report to the system active object.
 * @param me Video active object sending the report.
 * @param code Negative errorno_e value to include in the report.
 * @return None.
 */
static void post_error(video_ao_t* const me, int32_t code)
{
    app_error_evt_t* const error_evt = Q_NEW(app_error_evt_t, COMPONENT_ERROR_SIG);

    Q_UNUSED_PAR(me);

    error_evt->source = COMPONENT_VIDEO;
    error_evt->code = code;
    QACTIVE_POST(AO_System, &error_evt->super, &me->super);
}

/**
 * @brief Initialize the video pipeline and start periodic polling on success.
 * @param me Video active object receiving COMPONENT_INIT_SIG.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int on_component_init(video_ao_t* const me)
{
    int status = -EIO;

    if (video_pipeline_init(&me->pipeline) == 0)
    {
        me->now_ms = 0U;
        me->running = 1;
        post_ready(me);
        QTimeEvt_armX(&me->poll_time_evt, VIDEO_AO_POLL_TICKS, VIDEO_AO_POLL_TICKS);
        status = 0;
    }
    else
    {
        enter_error(me, -EIO);
    }

    return status;
}

/**
 * @brief Poll the video pipeline and report a component error if the pipeline fails.
 * @param me Video active object receiving VIDEO_POLL_SIG.
 * @return 0 when polling should continue, or a negative errorno_e value on failure.
 */
static int on_video_poll(video_ao_t* const me)
{
    video_pipeline_poll_result_e poll_result;
    int status = 0;

    if (me->running)
    {
        me->now_ms += VIDEO_AO_POLL_PERIOD_MS;
        poll_result = video_pipeline_poll(&me->pipeline, me->now_ms);
        if (poll_result == VIDEO_PIPELINE_POLL_ERROR)
        {
            enter_error(me, -EIO);
            status = -EIO;
        }
    }

    return status;
}

/**
 * @brief Stop periodic polling, clean up the pipeline, and report a video AO error.
 * @param me Video active object entering its terminal error state.
 * @param code Negative errorno_e value to post to system_ao_t.
 * @return None.
 */
static void enter_error(video_ao_t* const me, int32_t code)
{
    if (me->running)
    {
        (void)QTimeEvt_disarm(&me->poll_time_evt);
        video_pipeline_cleanup(&me->pipeline);
        me->running = 0;
    }

    post_error(me, code);
}

/**
 * @brief Run the initial transition for video_ao_t.
 * @param me Video active object instance.
 * @param par Optional initial transition parameter supplied by QP/C.
 * @return QP/C transition result.
 */
static QState video_ao_initial(video_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&video_ao_idle);
}

/**
 * @brief Handle component initialization before the video pipeline is running.
 * @param me Video active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState video_ao_idle(video_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_INIT_SIG:
        if (on_component_init(me) == 0)
        {
            status = Q_TRAN(&video_ao_running);
        }
        else
        {
            status = Q_TRAN(&video_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Handle periodic video pipeline polling after initialization succeeds.
 * @param me Video active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState video_ao_running(video_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case VIDEO_POLL_SIG:
        if (on_video_poll(me) == 0)
        {
            status = Q_HANDLED();
        }
        else
        {
            status = Q_TRAN(&video_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state reached after video pipeline initialization or polling fails.
 * @param me Video active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState video_ao_error(video_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    return Q_SUPER(&QHsm_top);
}

// === Public function implementation ============================================================================== //

/**
 * @brief Construct the video active object and its owned timer.
 * @param None.
 * @return None.
 */
void video_ao_ctor(void)
{
    video_ao_t* const me = &video_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&video_ao_initial));
    QTimeEvt_ctorX(&me->poll_time_evt, &me->super, VIDEO_POLL_SIG, 0U);
    me->now_ms = 0U;
    me->running = 0;
}

// === End of documentation ======================================================================================== //
