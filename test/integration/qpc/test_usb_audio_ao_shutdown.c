// Shutdown behaviour of usb_audio_ao_t.
//
// The property under test is that the AO never joins its capture worker from a
// state handler. Joining there would stall the cooperative QP/C thread -- and that
// same thread dispatches SystemAO's shutdown timeout, so a worker stuck inside a
// blocking ALSA read would deadlock the fail-fast shutdown it is supposed to bound.
// The AO must instead request the stop, acknowledge only once the worker reports
// completion, and reach its terminal state without ever blocking.

#include "unity.h"

#include "app.h"
#include "qpc_test_harness.h"
#include "USBAudioAO.h"

#include "mock_usb_audio_stream.h"

TEST_SOURCE_FILE("qpc_test_harness.c")
TEST_SOURCE_FILE("USBAudioAO.c")
TEST_SOURCE_FILE("log.c")
TEST_SOURCE_FILE("qf_port.c")
TEST_SOURCE_FILE("qep_hsm.c")
TEST_SOURCE_FILE("qf_act.c")
TEST_SOURCE_FILE("qf_actq.c")
TEST_SOURCE_FILE("qf_dyn.c")
TEST_SOURCE_FILE("qf_mem.c")
TEST_SOURCE_FILE("qf_qact.c")
TEST_SOURCE_FILE("qf_qeq.c")
TEST_SOURCE_FILE("qf_time.c")

static qpc_test_fake_ao_t system_fake;
static QEvtPtr system_queue[8];
static QEvtPtr usb_audio_queue[8];

QActive* const AO_System = &system_fake.super;

static void start_usb_audio_with_fake_system(void)
{
    qpc_test_init();

    qpc_test_fake_ao_ctor(&system_fake);
    qpc_test_start(AO_System, 1U, system_queue, Q_DIM(system_queue));

    usb_audio_ao_ctor();
    qpc_test_start(AO_USBAudio, 3U, usb_audio_queue, Q_DIM(usb_audio_queue));
}

// Drive the AO to its ready state with capture reported as started.
static void reach_ready_state(void)
{
    usb_audio_stream_default_config_Ignore();
    usb_audio_stream_start_IgnoreAndReturn(0);

    qpc_test_post_component_ready(AO_USBAudio, COMPONENT_USB_AUDIO, 0U, 0U);
    // COMPONENT_INIT is what the AO acts on; post it directly.
    {
        component_init_evt_t* const init =
            Q_NEW_X(component_init_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_INIT_SIG);

        init->source = COMPONENT_USB_AUDIO;
        init->width = 0U;
        init->height = 0U;
        (void)QACTIVE_POST_X(AO_USBAudio, &init->super, APP_CONTROL_EVENT_MARGIN, (void*)0);
    }
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);

    // Drain the ready report the AO sent to SystemAO.
    while (qpc_test_queue_use(AO_System) > 0U)
    {
        qpc_test_gc(qpc_test_pop(AO_System));
    }
}

static QEvt const* pop_system_event(void)
{
    TEST_ASSERT_TRUE_MESSAGE(qpc_test_queue_use(AO_System) > 0U,
                             "expected an event for SystemAO");
    return qpc_test_pop(AO_System);
}

static void assert_no_system_event(void)
{
    TEST_ASSERT_EQUAL_UINT16(0U, qpc_test_queue_use(AO_System));
}

void setUp(void)
{}

void tearDown(void)
{}

// A worker that has already exited: the AO may finish the stop immediately and
// acknowledge in the same dispatch, with no intermediate waiting state.
void test_system_stop_acks_immediately_when_the_worker_already_exited(void)
{
    QEvt const* event;

    start_usb_audio_with_fake_system();
    reach_ready_state();

    usb_audio_stream_request_stop_Ignore();
    usb_audio_stream_stop_complete_IgnoreAndReturn(1U);
    usb_audio_stream_finish_stop_IgnoreAndReturn(0);

    qpc_test_post_signal(AO_USBAudio, SYSTEM_STOP_SIG);
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);

    event = pop_system_event();
    TEST_ASSERT_EQUAL_UINT(SYSTEM_STOPPED_SIG, event->sig);
    TEST_ASSERT_EQUAL_INT(COMPONENT_USB_AUDIO, ((component_ready_evt_t const*)event)->source);
    qpc_test_gc(event);
}

// A worker still inside its loop: the AO must NOT acknowledge yet, and must not
// have called finish_stop -- the strict-ordering mock fails the test if it did,
// because a join here is exactly the blocking call this ticket removed.
void test_system_stop_defers_the_ack_while_the_worker_is_still_running(void)
{
    start_usb_audio_with_fake_system();
    reach_ready_state();

    usb_audio_stream_request_stop_Ignore();
    usb_audio_stream_stop_complete_IgnoreAndReturn(0U);

    qpc_test_post_signal(AO_USBAudio, SYSTEM_STOP_SIG);
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);

    assert_no_system_event();
}

// ...and once the worker reports completion on a later poll, the ack follows.
void test_stopping_state_acks_after_the_worker_reports_completion(void)
{
    QEvt const* event;

    start_usb_audio_with_fake_system();
    reach_ready_state();

    usb_audio_stream_request_stop_Ignore();
    usb_audio_stream_stop_complete_IgnoreAndReturn(0U);

    qpc_test_post_signal(AO_USBAudio, SYSTEM_STOP_SIG);
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);
    assert_no_system_event();

    // The worker has now left its loop; the next health poll observes it.
    usb_audio_stream_stop_complete_IgnoreAndReturn(1U);
    usb_audio_stream_finish_stop_IgnoreAndReturn(0);

    qpc_test_post_signal(AO_USBAudio, USB_AUDIO_POLL_SIG);
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);

    event = pop_system_event();
    TEST_ASSERT_EQUAL_UINT(SYSTEM_STOPPED_SIG, event->sig);
    qpc_test_gc(event);
}

// A repeated STOP while already stopping must be absorbed, not re-acknowledged:
// SystemAO tracks acks in a bitmask, but a second ack would still be a bug here.
void test_repeated_system_stop_while_stopping_is_ignored(void)
{
    start_usb_audio_with_fake_system();
    reach_ready_state();

    usb_audio_stream_request_stop_Ignore();
    usb_audio_stream_stop_complete_IgnoreAndReturn(0U);

    qpc_test_post_signal(AO_USBAudio, SYSTEM_STOP_SIG);
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);

    qpc_test_post_signal(AO_USBAudio, SYSTEM_STOP_SIG);
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);

    assert_no_system_event();
}

// A stop arriving before capture ever started has nothing to join, so it must
// acknowledge at once rather than parking in the waiting state forever.
void test_system_stop_before_init_acks_without_touching_the_worker(void)
{
    QEvt const* event;

    start_usb_audio_with_fake_system();

    usb_audio_stream_request_stop_Ignore();
    usb_audio_stream_stop_complete_IgnoreAndReturn(1U);
    usb_audio_stream_finish_stop_IgnoreAndReturn(0);

    qpc_test_post_signal(AO_USBAudio, SYSTEM_STOP_SIG);
    qpc_test_dispatch_until_idle(AO_USBAudio, 8U);

    event = pop_system_event();
    TEST_ASSERT_EQUAL_UINT(SYSTEM_STOPPED_SIG, event->sig);
    qpc_test_gc(event);
}
