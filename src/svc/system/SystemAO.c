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

    component_id_e last_ready_component;
    component_id_e error_source;
    uint32_t error_code;
} system_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState system_ao_initial(system_ao_t* const me, void const* const par);
static QState system_ao_active(system_ao_t* const me, QEvt const* const e);
static QState system_ao_init(system_ao_t* const me, QEvt const* const e);
static QState system_ao_run(system_ao_t* const me, QEvt const* const e);
static QState system_ao_error(system_ao_t* const me, QEvt const* const e);

static void on_init(system_ao_t* const me);
static bool on_component_ready(system_ao_t* const me, component_ready_evt_t const* const e);
static void on_run(system_ao_t* const me);
static void on_error(system_ao_t* const me, app_error_evt_t const* const e);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

static system_ao_t system_ao_inst;

// === Public variable definitions ================================================================================= //

QActive* const AO_System = Q_ACTIVE_UPCAST(&system_ao_inst);

// === Private function implementation ============================================================================= //

static void on_init(system_ao_t* const me)
{
    me->last_ready_component = COMPONENT_NONE;

    /*
     * TODO: When VideoAO exists, post COMPONENT_INIT_SIG to AO_Video here.
     * After each COMPONENT_READY_SIG, on_component_ready() will post the same
     * command to the next AO. After the final component is ready, transition
     * system_ao_t from system_ao_init to system_ao_run.
     */
}

static bool on_component_ready(system_ao_t* const me, component_ready_evt_t const* const e)
{
    me->last_ready_component = e->source;

    /*
     * TODO: Continue the sequential startup chain here:
     * COMPONENT_VIDEO             -> post COMPONENT_INIT_SIG to AO_USBAudio
     * COMPONENT_USB_AUDIO         -> post COMPONENT_INIT_SIG to AO_SubtitlePipeline
     * COMPONENT_SUBTITLE_PIPELINE -> post COMPONENT_INIT_SIG to AO_Buttons
     * COMPONENT_BUTTONS           -> post COMPONENT_INIT_SIG to AO_LED
     * COMPONENT_LED               -> transition system_ao_t to system_ao_run
     */

    return (e->source == COMPONENT_LED);
}

static void on_run(system_ao_t* const me)
{
    Q_UNUSED_PAR(me);

    // TODO: Notify interested AOs that normal application operation can begin.
}

static void on_error(system_ao_t* const me, app_error_evt_t const* const e)
{
    me->error_source = e->source;
    me->error_code = e->code;

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
        if (on_component_ready(me, Q_EVT_CAST(component_ready_evt_t)))
        {
            status = Q_TRAN(&system_ao_run);
        }
        else
        {
            status = Q_HANDLED();
        }
        break;

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
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    /*
     * This is intentionally a terminal state for the first milestone. Add a
     * reset or recovery signal later when the desired recovery policy exists.
     */
    return Q_SUPER(&QHsm_top);
}

// === Public function implementation ============================================================================== //

void system_ao_ctor(void)
{
    system_ao_t* const me = &system_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&system_ao_initial));
    me->last_ready_component = COMPONENT_NONE;
    me->error_source = COMPONENT_NONE;
    me->error_code = 0U;
}

// === End of documentation ======================================================================================== //
