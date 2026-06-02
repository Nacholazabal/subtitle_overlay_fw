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
#include "SystemAO.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;

    ComponentId lastReadyComponent;
    ComponentId errorSource;
    uint32_t errorCode;
} SystemAO;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState SystemAO_initial(SystemAO* const me, void const* const par);
static QState SystemAO_active(SystemAO* const me, QEvt const* const e);
static QState SystemAO_init(SystemAO* const me, QEvt const* const e);
static QState SystemAO_run(SystemAO* const me, QEvt const* const e);
static QState SystemAO_error(SystemAO* const me, QEvt const* const e);

static void on_init(SystemAO* const me);
static bool on_component_ready(SystemAO* const me, ComponentReadyEvt const* const e);
static void on_run(SystemAO* const me);
static void on_error(SystemAO* const me, AppErrorEvt const* const e);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

static SystemAO SystemAO_inst;

// === Public variable definitions ================================================================================= //

QActive* const AO_System = Q_ACTIVE_UPCAST(&SystemAO_inst);

// === Private function implementation ============================================================================= //

static void on_init(SystemAO* const me)
{
    me->lastReadyComponent = COMPONENT_NONE;

    /*
     * TODO: When VideoAO exists, post COMPONENT_INIT_SIG to AO_Video here.
     * After each COMPONENT_READY_SIG, on_component_ready() will post the same
     * command to the next AO. After the final component is ready, transition
     * SystemAO from SystemAO_init to SystemAO_run.
     */
}

static bool on_component_ready(SystemAO* const me, ComponentReadyEvt const* const e)
{
    me->lastReadyComponent = e->source;

    /*
     * TODO: Continue the sequential startup chain here:
     * COMPONENT_VIDEO             -> post COMPONENT_INIT_SIG to AO_USBAudio
     * COMPONENT_USB_AUDIO         -> post COMPONENT_INIT_SIG to AO_SubtitlePipeline
     * COMPONENT_SUBTITLE_PIPELINE -> post COMPONENT_INIT_SIG to AO_Buttons
     * COMPONENT_BUTTONS           -> post COMPONENT_INIT_SIG to AO_LED
     * COMPONENT_LED               -> transition SystemAO to SystemAO_run
     */

    return (e->source == COMPONENT_LED);
}

static void on_run(SystemAO* const me)
{
    Q_UNUSED_PAR(me);

    // TODO: Notify interested AOs that normal application operation can begin.
}

static void on_error(SystemAO* const me, AppErrorEvt const* const e)
{
    me->errorSource = e->source;
    me->errorCode = e->code;

    // TODO: Report the fault through logging and request an LED error indication.
}

static QState SystemAO_initial(SystemAO* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&SystemAO_init);
}

static QState SystemAO_active(SystemAO* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_ERROR_SIG:
        on_error(me, Q_EVT_CAST(AppErrorEvt));
        status = Q_TRAN(&SystemAO_error);
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

static QState SystemAO_init(SystemAO* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        on_init(me);
        status = Q_HANDLED();
        break;

    case COMPONENT_READY_SIG:
        if (on_component_ready(me, Q_EVT_CAST(ComponentReadyEvt)))
        {
            status = Q_TRAN(&SystemAO_run);
        }
        else
        {
            status = Q_HANDLED();
        }
        break;

    default:
        status = Q_SUPER(&SystemAO_active);
        break;
    }

    return status;
}

static QState SystemAO_run(SystemAO* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        on_run(me);
        status = Q_HANDLED();
        break;

    default:
        status = Q_SUPER(&SystemAO_active);
        break;
    }

    return status;
}

static QState SystemAO_error(SystemAO* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    /*
     * This is intentionally a terminal state for the first milestone. Add a
     * reset or recovery signal later when the desired recovery policy exists.
     */
    return Q_SUPER(&QHsm_top);
}

// === Public function implementation ============================================================================== //

void SystemAO_ctor(void)
{
    SystemAO* const me = &SystemAO_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&SystemAO_initial));
    me->lastReadyComponent = COMPONENT_NONE;
    me->errorSource = COMPONENT_NONE;
    me->errorCode = 0U;
}

// === End of documentation ======================================================================================== //
