#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "qpc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    QActive super;
} qpc_test_fake_ao_t;

void qpc_test_init(void);
void qpc_test_fake_ao_ctor(qpc_test_fake_ao_t* fake);
void qpc_test_start(QActive* ao, uint_fast8_t prio, QEvtPtr* queue, uint_fast16_t queue_len);
void qpc_test_dispatch_one(QActive* ao);
void qpc_test_dispatch_until_idle(QActive* ao, uint32_t max_events);
QEvt const* qpc_test_pop(QActive* ao);
void qpc_test_gc(QEvt const* event);
uint16_t qpc_test_queue_use(QActive const* ao);
void qpc_test_post_component_ready(QActive* target,
                                   component_id_e source,
                                   uint32_t width,
                                   uint32_t height);
void qpc_test_post_component_error(QActive* target, component_id_e source, int32_t code);
void qpc_test_post_subtitle_text(QActive* target,
                                 uint32_t seq,
                                 uint8_t is_final,
                                 char const* text);
void qpc_test_post_signal(QActive* target, QSignal sig);

#ifdef __cplusplus
}
#endif
