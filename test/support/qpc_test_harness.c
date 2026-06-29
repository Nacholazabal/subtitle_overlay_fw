#define QP_IMPL

#include "qpc_test_harness.h"

#include "qp_port.h"
#include "qp_pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define QPC_TEST_POOL_LEN (64U)

typedef union
{
    component_init_evt_t component_init;
    component_ready_evt_t component_ready;
    app_error_evt_t app_error;
    subtitle_text_evt_t subtitle_text;
} qpc_test_pool_evt_t;

static QF_MPOOL_EL(qpc_test_pool_evt_t) test_pool[QPC_TEST_POOL_LEN];

static QState fake_ao_initial(qpc_test_fake_ao_t* const me, void const* const par);
static QState fake_ao_active(qpc_test_fake_ao_t* const me, QEvt const* const e);

static QState fake_ao_initial(qpc_test_fake_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&fake_ao_active);
}

static QState fake_ao_active(qpc_test_fake_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);

    return (e->sig == Q_ENTRY_SIG) ? Q_HANDLED() : Q_SUPER(&QHsm_top);
}

void qpc_test_init(void)
{
    memset(QActive_registry_, 0, sizeof(QActive_registry_));
    memset(&QF_priv_, 0, sizeof(QF_priv_));
    QPSet_setEmpty(&QF_readySet_);

    log_init();
    QF_init();
    QF_setTickRate(0U, 0);
    QF_poolInit(test_pool, sizeof(test_pool), sizeof(test_pool[0]));
}

void qpc_test_fake_ao_ctor(qpc_test_fake_ao_t* const fake)
{
    QActive_ctor(&fake->super, Q_STATE_CAST(&fake_ao_initial));
}

void qpc_test_start(QActive* const ao,
                    uint_fast8_t prio,
                    QEvtPtr* const queue,
                    uint_fast16_t queue_len)
{
    QActive_start(ao, prio, queue, queue_len, (void*)0, 0U, (void*)0);
}

void qpc_test_dispatch_one(QActive* const ao)
{
    QEvt const* const event = QActive_get_(ao);
    QASM_DISPATCH(ao, event, ao->prio);
    QF_gc(event);

    if (ao->eQueue.frontEvt.e == (QEvt*)0)
    {
        QPSet_remove(&QF_readySet_, ao->prio);
    }
}

void qpc_test_dispatch_until_idle(QActive* const ao, uint32_t max_events)
{
    while ((ao->eQueue.frontEvt.e != (QEvt*)0) && (max_events > 0U))
    {
        qpc_test_dispatch_one(ao);
        max_events--;
    }
}

QEvt const* qpc_test_pop(QActive* const ao)
{
    QEvt const* const event = QActive_get_(ao);

    if (ao->eQueue.frontEvt.e == (QEvt*)0)
    {
        QPSet_remove(&QF_readySet_, ao->prio);
    }

    return event;
}

void qpc_test_gc(QEvt const* const event)
{
    QF_gc(event);
}

uint16_t qpc_test_queue_use(QActive const* const ao)
{
    return QEQueue_getUse(&ao->eQueue);
}

void qpc_test_post_component_ready(QActive* const target,
                                   component_id_e source,
                                   uint32_t width,
                                   uint32_t height)
{
    component_ready_evt_t* const event =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_READY_SIG);

    event->source = source;
    event->width = width;
    event->height = height;
    (void)QACTIVE_POST_X(target, &event->super, APP_CONTROL_EVENT_MARGIN, (void*)0);
}

void qpc_test_post_component_error(QActive* const target, component_id_e source, int32_t code)
{
    app_error_evt_t* const event =
        Q_NEW_X(app_error_evt_t, APP_ERROR_EVENT_MARGIN, COMPONENT_ERROR_SIG);

    event->source = source;
    event->code = code;
    (void)QACTIVE_POST_X(target, &event->super, APP_ERROR_EVENT_MARGIN, (void*)0);
}

void qpc_test_post_subtitle_text(QActive* const target,
                                 uint32_t seq,
                                 uint8_t is_final,
                                 char const* const text)
{
    subtitle_text_evt_t* const event =
        Q_NEW_X(subtitle_text_evt_t, APP_ERROR_EVENT_MARGIN, SUBTITLE_TEXT_SIG);

    event->seq = seq;
    event->start_ms = seq * 100U;
    event->end_ms = event->start_ms + 50U;
    event->is_final = is_final;
    snprintf(event->text, sizeof(event->text), "%s", text);
    (void)QACTIVE_POST_X(target, &event->super, APP_ERROR_EVENT_MARGIN, (void*)0);
}

void qpc_test_post_signal(QActive* const target, QSignal sig)
{
    QEvt* const event = Q_NEW_X(QEvt, APP_ERROR_EVENT_MARGIN, sig);

    (void)QACTIVE_POST_X(target, event, APP_ERROR_EVENT_MARGIN, (void*)0);
}

Q_NORETURN Q_onError(char const* const module, int_t const id)
{
    fprintf(stderr, "QP/C test assertion failed in %s:%d\n", module, id);
    abort();
}

void QF_onStartup(void)
{}

void QF_onCleanup(void)
{}

void QF_onClockTick(void)
{
    QTIMEEVT_TICK_X(0U, (void*)0);
}
