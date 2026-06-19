/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file VideoAO.c
/// @brief Video pipeline orchestration active object
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

#include "app.h"
#include "log.h"
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
    uint8_t running;
    uint8_t ready_posted;
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
    component_ready_evt_t* const ready_evt =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_READY_SIG);
    video_pipeline_mode_t const* const mode = video_pipeline_get_active_mode(&me->pipeline);

    if (ready_evt == NULL)
    {
        LOG_ERROR("video: failed to allocate ready event");
        return;
    }

    ready_evt->width = 0U;
    ready_evt->height = 0U;
    if (mode != NULL)
    {
        ready_evt->width = mode->timing.width;
        ready_evt->height = mode->timing.height;
    }
    ready_evt->source = COMPONENT_VIDEO;
    if (!QACTIVE_POST_X(AO_System, &ready_evt->super, APP_CONTROL_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("video: failed to post ready event");
    }
}

/**
 * @brief Post a component-error report to the system active object.
 * @param me Video active object sending the report.
 * @param code Negative errno-style value to include in the report.
 * @return None.
 */
static void post_error(video_ao_t* const me, int32_t code)
{
    app_error_evt_t* const error_evt =
        Q_NEW_X(app_error_evt_t, APP_ERROR_EVENT_MARGIN, COMPONENT_ERROR_SIG);

    Q_UNUSED_PAR(me);

    if (error_evt == NULL)
    {
        LOG_ERROR("video: failed to allocate error event, code=%ld", (long)code);
        return;
    }

    error_evt->source = COMPONENT_VIDEO;
    error_evt->code = code;
    if (!QACTIVE_POST_X(AO_System, &error_evt->super, APP_ERROR_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("video: failed to post error event, code=%ld", (long)code);
    }
}

/**
 * @brief Initialize the video pipeline and start periodic polling on success.
 * @param me Video active object receiving COMPONENT_INIT_SIG.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int on_component_init(video_ao_t* const me)
{
    int status = -EIO;

    LOG_INFO("video: initializing pipeline");

    if (video_pipeline_init(&me->pipeline) == 0)
    {
        me->now_ms = 0U;
        me->running = 1U;
        me->ready_posted = 0U;
        QTimeEvt_armX(&me->poll_time_evt, VIDEO_AO_POLL_TICKS, VIDEO_AO_POLL_TICKS);
        LOG_INFO("video: pipeline running, poll period=%u ms", (unsigned)VIDEO_AO_POLL_PERIOD_MS);
        status = 0;
    }
    else
    {
        LOG_ERROR("video: pipeline init failed");
        enter_error(me, -EIO);
    }

    return status;
}

/**
 * @brief Poll the video pipeline and report a component error if the pipeline fails.
 * @param me Video active object receiving VIDEO_POLL_SIG.
 * @return 0 when polling should continue, or a negative errno-style value on failure.
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
            LOG_ERROR("video: pipeline poll failed at %lu ms", (unsigned long)me->now_ms);
            enter_error(me, -EIO);
            status = -EIO;
        }
        else if ((poll_result == VIDEO_PIPELINE_POLL_STREAMING_STARTED) && !me->ready_posted)
        {
            video_pipeline_mode_t const* const mode = video_pipeline_get_active_mode(&me->pipeline);

            if (mode == NULL)
            {
                LOG_ERROR("video: pipeline reported streaming without active mode");
                enter_error(me, -EIO);
                status = -EIO;
            }
            else
            {
                me->ready_posted = 1U;
                post_ready(me);
                LOG_INFO("video: passthrough streaming %lux%lu",
                         (unsigned long)mode->timing.width,
                         (unsigned long)mode->timing.height);
            }
        }
    }

    return status;
}

/**
 * @brief Stop periodic polling, clean up the pipeline, and report a video AO error.
 * @param me Video active object entering its terminal error state.
 * @param code Negative errno-style value to post to system_ao_t.
 * @return None.
 */
static void enter_error(video_ao_t* const me, int32_t code)
{
    if (me->running)
    {
        LOG_WARNING("video: stopping pipeline after error code %ld", (long)code);
        (void)QTimeEvt_disarm(&me->poll_time_evt);
        video_pipeline_cleanup(&me->pipeline);
        me->running = 0U;
        me->ready_posted = 0U;
    }
    else
    {
        LOG_ERROR("video: entering error state before pipeline was running, code=%ld", (long)code);
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
    me->running = 0U;
    me->ready_posted = 0U;
}

// === End of documentation ======================================================================================== //
