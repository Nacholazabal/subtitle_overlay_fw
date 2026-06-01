/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file template_qpc_AO.c
/// @brief Template source file for a QP/C active object
///

// === Headers files inclusions ==================================================================================== //

/*
 * Copy this file into src/svc/, rename TemplateAO consistently, and enable the
 * worksheet below. AO code should request hardware work through hal/ or bsp/
 * APIs instead of touching registers or Linux drivers directly.
 */

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

#if 0
#include "qpc.h"
#include "app.h"
// #include "hal/example_hal.h"  // Optional hardware adapter.

// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;

    // Add AO-owned state here.
    // QTimeEvt timeEvt;  // Optional: construct in TemplateAO_ctor().
} TemplateAO;

// === Private function declarations =============================================================================== //

static QState TemplateAO_initial(TemplateAO * const me, void const * const par);
static QState TemplateAO_state(TemplateAO * const me, QEvt const * const e);

// === Private variable definitions ================================================================================ //

static TemplateAO TemplateAO_inst;

// === Public variable definitions ================================================================================= //

QActive * const AO_Template = Q_ACTIVE_UPCAST(&TemplateAO_inst);

// === Public function implementation ============================================================================== //

void TemplateAO_ctor(void)
{
    TemplateAO * const me = &TemplateAO_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&TemplateAO_initial));

    // Initialize AO-owned members here.
    // QTimeEvt_ctorX(&me->timeEvt, &me->super, TEMPLATE_TIMEOUT_SIG, 0U);
}

// === Private function implementation ============================================================================= //

static QState TemplateAO_initial(TemplateAO * const me, void const * const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    // Optional tracing dictionaries:
    // QS_OBJ_DICTIONARY(&TemplateAO_inst);
    // QS_FUN_DICTIONARY(&TemplateAO_state);

    // Optional subscriptions for published facts:
    // QActive_subscribe(Q_ACTIVE_UPCAST(me), APP_STARTED_SIG);

    // Optional timer startup; use interval 0U for one-shot timers:
    // QTimeEvt_armX(&me->timeEvt, INITIAL_TICKS, PERIODIC_INTERVAL_TICKS);

    return Q_TRAN(&TemplateAO_state);
}

static QState TemplateAO_state(TemplateAO * const me, QEvt const * const e)
{
    QState status;

    switch (e->sig) {
    case Q_ENTRY_SIG:
        // Enter-state actions go here.
        status = Q_HANDLED();
        break;

    case Q_EXIT_SIG:
        // Exit-state actions go here. Disarm state-scoped timers if needed.
        // QTimeEvt_disarm(&me->timeEvt);
        status = Q_HANDLED();
        break;

    case TEMPLATE_EVENT_SIG:
        // Handle event data, post a directed command, publish a fact, or transition:
        // QACTIVE_POST(AO_Other, e, Q_ACTIVE_UPCAST(me));
        // QACTIVE_PUBLISH(e, Q_ACTIVE_UPCAST(me));
        // status = Q_TRAN(&TemplateAO_otherState);
        status = Q_HANDLED();
        break;

    default:
        // Replace QHsm_top with a parent state when implementing a substate.
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}
#endif

// === End of documentation ======================================================================================== //
