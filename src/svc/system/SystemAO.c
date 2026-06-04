/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file SystemAO.c
/// @brief System orchestration active object
///

// === Headers files inclusions ==================================================================================== //

#include "qpc.h"

#include "app.h"
#include "log.h"
#include "SubtitleAO.h"
#include "SystemAO.h"
#include "VideoAO.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;

    component_id_e last_ready_component;
    component_id_e error_source;
    int32_t error_code;
    uint32_t active_video_width;
    uint32_t active_video_height;
} system_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState system_ao_initial(system_ao_t* const me, void const* const par);
static QState system_ao_active(system_ao_t* const me, QEvt const* const e);
static QState system_ao_init(system_ao_t* const me, QEvt const* const e);
static QState system_ao_run(system_ao_t* const me, QEvt const* const e);
static QState system_ao_error(system_ao_t* const me, QEvt const* const e);

static void post_component_init(system_ao_t* const me,
                                QActive* const target,
                                component_id_e source,
                                uint32_t width,
                                uint32_t height);
static void on_init(system_ao_t* const me);
static int on_component_ready(system_ao_t* const me, component_ready_evt_t const* const e);
static void on_run(system_ao_t* const me);
static void on_error(system_ao_t* const me, app_error_evt_t const* const e);
static const char* component_id_to_str(component_id_e component);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

static system_ao_t system_ao_inst;

// === Public variable definitions ================================================================================= //

QActive* const AO_System = Q_ACTIVE_UPCAST(&system_ao_inst);

// === Private function implementation ============================================================================= //

static const char* component_id_to_str(component_id_e component)
{
    switch (component)
    {
    case COMPONENT_VIDEO:
        return "video";

    case COMPONENT_USB_AUDIO:
        return "usb-audio";

    case COMPONENT_SUBTITLE_PIPELINE:
        return "subtitle";

    case COMPONENT_BUTTONS:
        return "buttons";

    case COMPONENT_LED:
        return "led";

    case COMPONENT_NONE:
    default:
        return "unknown";
    }
}

static void post_component_init(system_ao_t* const me,
                                QActive* const target,
                                component_id_e source,
                                uint32_t width,
                                uint32_t height)
{
    component_init_evt_t* const init_evt = Q_NEW(component_init_evt_t, COMPONENT_INIT_SIG);

    Q_UNUSED_PAR(me);

    init_evt->source = source;
    init_evt->width = width;
    init_evt->height = height;
    QACTIVE_POST(target, &init_evt->super, &me->super);
}

static void on_init(system_ao_t* const me)
{
    me->last_ready_component = COMPONENT_NONE;
    me->active_video_width = 0U;
    me->active_video_height = 0U;

    LOG_INFO("system: init sequence started");
    post_component_init(me, AO_Video, COMPONENT_NONE, 0U, 0U);
}

static int on_component_ready(system_ao_t* const me, component_ready_evt_t const* const e)
{
    int status = -EAGAIN;

    me->last_ready_component = e->source;
    LOG_INFO("system: component ready: %s", component_id_to_str(e->source));

    switch (e->source)
    {
    case COMPONENT_VIDEO:
        if ((e->width == 0U) || (e->height == 0U))
        {
            me->error_source = COMPONENT_VIDEO;
            me->error_code = -EINVAL;
            LOG_ERROR("system: video ready event has invalid dimensions %lux%lu",
                      (unsigned long)e->width,
                      (unsigned long)e->height);
            status = -EINVAL;
            break;
        }

        me->active_video_width = e->width;
        me->active_video_height = e->height;
        LOG_INFO("system: requesting subtitle init for %lux%lu",
                 (unsigned long)e->width,
                 (unsigned long)e->height);
        post_component_init(me, AO_Subtitle, COMPONENT_VIDEO, e->width, e->height);
        break;

    case COMPONENT_SUBTITLE_PIPELINE:
        status = 0;
        break;

    default:
        LOG_WARNING("system: ignoring unexpected ready source: %d", (int)e->source);
        break;
    }

    return status;
}

static void on_run(system_ao_t* const me)
{
    Q_UNUSED_PAR(me);

    LOG_INFO("system: all required components are running");

    // TODO: Notify interested AOs that normal application operation can begin.
}

static void on_error(system_ao_t* const me, app_error_evt_t const* const e)
{
    me->error_source = e->source;
    me->error_code = e->code;

    LOG_ERROR("system: component error from %s: %ld",
              component_id_to_str(e->source),
              (long)e->code);

    // TODO: Report the fault through logging and request an LED error indication.
}

static QState system_ao_initial(system_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&system_ao_init);
}

static QState system_ao_active(system_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_ERROR_SIG:
        on_error(me, Q_EVT_CAST(app_error_evt_t));
        status = Q_TRAN(&system_ao_error);
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

static QState system_ao_init(system_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        on_init(me);
        status = Q_HANDLED();
        break;

    case COMPONENT_READY_SIG:
    {
        int const ready_status = on_component_ready(me, Q_EVT_CAST(component_ready_evt_t));

        if (ready_status == 0)
        {
            status = Q_TRAN(&system_ao_run);
        }
        else if (ready_status == -EAGAIN)
        {
            status = Q_HANDLED();
        }
        else
        {
            status = Q_TRAN(&system_ao_error);
        }
        break;
    }

    default:
        status = Q_SUPER(&system_ao_active);
        break;
    }

    return status;
}

static QState system_ao_run(system_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        on_run(me);
        status = Q_HANDLED();
        break;

    default:
        status = Q_SUPER(&system_ao_active);
        break;
    }

    return status;
}

static QState system_ao_error(system_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        LOG_ERROR("system: terminal error state source=%s code=%ld",
                  component_id_to_str(me->error_source),
                  (long)me->error_code);
        status = Q_HANDLED();
        break;

    default:
        /*
         * This is intentionally a terminal state for the first milestone. Add a
         * reset or recovery signal later when the desired recovery policy exists.
         */
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

// === Public function implementation ============================================================================== //

void system_ao_ctor(void)
{
    system_ao_t* const me = &system_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&system_ao_initial));
    me->last_ready_component = COMPONENT_NONE;
    me->error_source = COMPONENT_NONE;
    me->error_code = 0;
    me->active_video_width = 0U;
    me->active_video_height = 0U;
}

// === End of documentation ======================================================================================== //
