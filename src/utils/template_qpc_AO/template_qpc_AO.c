/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file template_qpc_AO.c
/// @brief Template source file for a QP/C active object
///

// === Headers files inclusions ==================================================================================== //

/*
 * Copy this file into src/svc/, rename template_ao_t consistently, and enable the
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
    // QTimeEvt time_evt;  // Optional: construct in template_ao_ctor().
} template_ao_t;

// === Private function declarations =============================================================================== //

static QState template_ao_initial(template_ao_t * const me, void const * const par);
static QState template_ao_state(template_ao_t * const me, QEvt const * const e);

// === Private variable definitions ================================================================================ //

static template_ao_t template_ao_inst;

// === Public variable definitions ================================================================================= //

QActive * const AO_Template = Q_ACTIVE_UPCAST(&template_ao_inst);

// === Public function implementation ============================================================================== //

void template_ao_ctor(void)
{
    template_ao_t * const me = &template_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&template_ao_initial));

    // Initialize AO-owned members here.
    // QTimeEvt_ctorX(&me->time_evt, &me->super, TEMPLATE_TIMEOUT_SIG, 0U);
}

// === Private function implementation ============================================================================= //

static QState template_ao_initial(template_ao_t * const me, void const * const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    // Optional tracing dictionaries:
    // QS_OBJ_DICTIONARY(&template_ao_inst);
    // QS_FUN_DICTIONARY(&template_ao_state);

    // Optional subscriptions for published facts:
    // QActive_subscribe(Q_ACTIVE_UPCAST(me), APP_STARTED_SIG);

    // Optional timer startup; use interval 0U for one-shot timers:
    // QTimeEvt_armX(&me->time_evt, INITIAL_TICKS, PERIODIC_INTERVAL_TICKS);

    return Q_TRAN(&template_ao_state);
}

static QState template_ao_state(template_ao_t * const me, QEvt const * const e)
{
    QState status;

    switch (e->sig) {
    case Q_ENTRY_SIG:
        // Enter-state actions go here.
        status = Q_HANDLED();
        break;

    case Q_EXIT_SIG:
        // Exit-state actions go here. Disarm state-scoped timers if needed.
        // QTimeEvt_disarm(&me->time_evt);
        status = Q_HANDLED();
        break;

    case TEMPLATE_EVENT_SIG:
        // Handle event data, post a directed command, publish a fact, or transition:
        // QACTIVE_POST(AO_Other, e, Q_ACTIVE_UPCAST(me));
        // QACTIVE_PUBLISH(e, Q_ACTIVE_UPCAST(me));
        // status = Q_TRAN(&template_ao_other_state);
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
