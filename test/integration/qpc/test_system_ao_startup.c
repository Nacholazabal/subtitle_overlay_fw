#include "unity.h"

#include "app.h"
#include "qpc_test_harness.h"
#include "SystemAO.h"

TEST_SOURCE_FILE("qpc_test_harness.c")
TEST_SOURCE_FILE("SystemAO.c")
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

static qpc_test_fake_ao_t video_fake;
static qpc_test_fake_ao_t usb_audio_fake;
static qpc_test_fake_ao_t subtitle_fake;
static qpc_test_fake_ao_t stt_fake;
static QEvtPtr video_queue[8];
static QEvtPtr usb_audio_queue[8];
static QEvtPtr subtitle_queue[8];
static QEvtPtr stt_queue[8];
static QEvtPtr system_queue[8];

QActive* const AO_Video = &video_fake.super;
QActive* const AO_USBAudio = &usb_audio_fake.super;
QActive* const AO_Subtitle = &subtitle_fake.super;
QActive* const AO_Stt = &stt_fake.super;

static void start_system_with_fake_components(void)
{
    qpc_test_init();

    qpc_test_fake_ao_ctor(&video_fake);
    qpc_test_fake_ao_ctor(&usb_audio_fake);
    qpc_test_fake_ao_ctor(&subtitle_fake);
    qpc_test_fake_ao_ctor(&stt_fake);

    qpc_test_start(AO_Video, 2U, video_queue, Q_DIM(video_queue));
    qpc_test_start(AO_USBAudio, 3U, usb_audio_queue, Q_DIM(usb_audio_queue));
    qpc_test_start(AO_Subtitle, 4U, subtitle_queue, Q_DIM(subtitle_queue));
    qpc_test_start(AO_Stt, 5U, stt_queue, Q_DIM(stt_queue));

    system_ao_ctor();
    qpc_test_start(AO_System, 1U, system_queue, Q_DIM(system_queue));
}

static component_init_evt_t const* pop_component_init(QActive* const ao)
{
    QEvt const* const event = qpc_test_pop(ao);

    TEST_ASSERT_EQUAL_UINT(COMPONENT_INIT_SIG, event->sig);
    return (component_init_evt_t const*)event;
}

static void assert_no_event(QActive const* const ao)
{
    TEST_ASSERT_EQUAL_UINT16(0U, qpc_test_queue_use(ao));
}

void setUp(void)
{}

void tearDown(void)
{}

void test_system_start_posts_video_and_usb_audio_init(void)
{
    component_init_evt_t const* init;

    start_system_with_fake_components();

    init = pop_component_init(AO_Video);
    TEST_ASSERT_EQUAL_INT(COMPONENT_VIDEO, init->source);
    TEST_ASSERT_EQUAL_UINT32(0U, init->width);
    TEST_ASSERT_EQUAL_UINT32(0U, init->height);
    qpc_test_gc(&init->super);

    init = pop_component_init(AO_USBAudio);
    TEST_ASSERT_EQUAL_INT(COMPONENT_USB_AUDIO, init->source);
    TEST_ASSERT_EQUAL_UINT32(0U, init->width);
    TEST_ASSERT_EQUAL_UINT32(0U, init->height);
    qpc_test_gc(&init->super);

    assert_no_event(AO_Subtitle);
    assert_no_event(AO_Stt);
}

void test_system_waits_for_video_when_usb_audio_ready_first(void)
{
    component_init_evt_t const* init;

    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));

    qpc_test_post_component_ready(AO_System, COMPONENT_USB_AUDIO, 0U, 0U);
    qpc_test_dispatch_one(AO_System);
    assert_no_event(AO_Subtitle);

    qpc_test_post_component_ready(AO_System, COMPONENT_VIDEO, 1280U, 720U);
    qpc_test_dispatch_one(AO_System);

    init = pop_component_init(AO_Subtitle);
    TEST_ASSERT_EQUAL_INT(COMPONENT_SUBTITLE_PIPELINE, init->source);
    TEST_ASSERT_EQUAL_UINT32(1280U, init->width);
    TEST_ASSERT_EQUAL_UINT32(720U, init->height);
    qpc_test_gc(&init->super);
}

void test_system_waits_for_usb_audio_when_video_ready_first(void)
{
    component_init_evt_t const* init;

    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));

    qpc_test_post_component_ready(AO_System, COMPONENT_VIDEO, 1920U, 1080U);
    qpc_test_dispatch_one(AO_System);
    assert_no_event(AO_Subtitle);

    qpc_test_post_component_ready(AO_System, COMPONENT_USB_AUDIO, 0U, 0U);
    qpc_test_dispatch_one(AO_System);

    init = pop_component_init(AO_Subtitle);
    TEST_ASSERT_EQUAL_INT(COMPONENT_SUBTITLE_PIPELINE, init->source);
    TEST_ASSERT_EQUAL_UINT32(1920U, init->width);
    TEST_ASSERT_EQUAL_UINT32(1080U, init->height);
    qpc_test_gc(&init->super);
}

void test_system_ignores_duplicate_video_ready_after_subtitle_requested(void)
{
    // SRC-M04: a second COMPONENT_VIDEO ready must not post a second subtitle
    // init once the first has already been requested.
    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));

    qpc_test_post_component_ready(AO_System, COMPONENT_USB_AUDIO, 0U, 0U);
    qpc_test_dispatch_one(AO_System);
    qpc_test_post_component_ready(AO_System, COMPONENT_VIDEO, 1280U, 720U);
    qpc_test_dispatch_one(AO_System);

    // First subtitle init consumed.
    qpc_test_gc(qpc_test_pop(AO_Subtitle));

    // Duplicate video ready must be idempotent: no new subtitle init.
    qpc_test_post_component_ready(AO_System, COMPONENT_VIDEO, 1280U, 720U);
    qpc_test_dispatch_one(AO_System);
    assert_no_event(AO_Subtitle);
}

void test_system_requests_stt_after_subtitle_ready_and_runs_after_stt_ready(void)
{
    component_init_evt_t const* init;

    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));

    qpc_test_post_component_ready(AO_System, COMPONENT_USB_AUDIO, 0U, 0U);
    qpc_test_dispatch_one(AO_System);
    qpc_test_post_component_ready(AO_System, COMPONENT_VIDEO, 1280U, 720U);
    qpc_test_dispatch_one(AO_System);
    qpc_test_gc(qpc_test_pop(AO_Subtitle));

    qpc_test_post_component_ready(AO_System, COMPONENT_SUBTITLE_PIPELINE, 1280U, 720U);
    qpc_test_dispatch_one(AO_System);

    init = pop_component_init(AO_Stt);
    TEST_ASSERT_EQUAL_INT(COMPONENT_STT, init->source);
    TEST_ASSERT_EQUAL_UINT32(0U, init->width);
    TEST_ASSERT_EQUAL_UINT32(0U, init->height);
    qpc_test_gc(&init->super);

    qpc_test_post_component_ready(AO_System, COMPONENT_STT, 0U, 0U);
    qpc_test_dispatch_one(AO_System);

    assert_no_event(AO_Video);
    assert_no_event(AO_USBAudio);
    assert_no_event(AO_Subtitle);
    assert_no_event(AO_Stt);
}

static void assert_stop_signal(QActive* const ao)
{
    QEvt const* const event = qpc_test_pop(ao);

    TEST_ASSERT_EQUAL_UINT(SYSTEM_STOP_SIG, event->sig);
    qpc_test_gc(event); // static event: QF_gc is a safe no-op on it
}

static void assert_stop_broadcast_to_all_workers(void)
{
    assert_stop_signal(AO_Video);
    assert_stop_signal(AO_USBAudio);
    assert_stop_signal(AO_Subtitle);
    assert_stop_signal(AO_Stt);
}

// SRC-H07: an invalid video-ready is a terminal failure -> fail-fast shutdown.
void test_system_fail_fast_broadcasts_stop_on_invalid_video_ready(void)
{
    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));

    qpc_test_post_component_ready(AO_System, COMPONENT_VIDEO, 0U, 0U);
    qpc_test_dispatch_one(AO_System);

    assert_stop_broadcast_to_all_workers();
}

// SRC-H07: a component error triggers the coordinated fail-fast shutdown.
void test_system_fail_fast_broadcasts_stop_on_component_error(void)
{
    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));

    qpc_test_post_component_error(AO_System, COMPONENT_USB_AUDIO, -EIO);
    qpc_test_dispatch_one(AO_System);

    assert_stop_broadcast_to_all_workers();

    // Already shutting down: further component traffic is ignored (no re-broadcast).
    qpc_test_post_component_ready(AO_System, COMPONENT_USB_AUDIO, 0U, 0U);
    qpc_test_dispatch_one(AO_System);
    assert_no_event(AO_Video);
    assert_no_event(AO_USBAudio);
}

void test_system_shutdown_signal_broadcasts_a_normal_coordinated_stop(void)
{
    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));
    qpc_test_post_signal(AO_System, SYSTEM_SHUTDOWN_SIG);
    qpc_test_dispatch_one(AO_System);

    assert_stop_broadcast_to_all_workers();
}

// SRC-H07: once every worker acknowledges SYSTEM_STOPPED, the shutdown completes
// (system_ao_t requests QF termination) without hanging.
void test_system_shutdown_completes_after_all_workers_stop(void)
{
    start_system_with_fake_components();

    qpc_test_gc(qpc_test_pop(AO_Video));
    qpc_test_gc(qpc_test_pop(AO_USBAudio));

    qpc_test_post_component_error(AO_System, COMPONENT_VIDEO, -EIO);
    qpc_test_dispatch_one(AO_System);
    assert_stop_broadcast_to_all_workers();

    // Each worker acks; System must accept all four without error.
    qpc_test_post_component_stopped_ack(AO_System, COMPONENT_VIDEO);
    qpc_test_dispatch_one(AO_System);
    qpc_test_post_component_stopped_ack(AO_System, COMPONENT_USB_AUDIO);
    qpc_test_dispatch_one(AO_System);
    qpc_test_post_component_stopped_ack(AO_System, COMPONENT_SUBTITLE_PIPELINE);
    qpc_test_dispatch_one(AO_System);
    qpc_test_post_component_stopped_ack(AO_System, COMPONENT_STT);
    qpc_test_dispatch_one(AO_System);

    // Shutdown handshake consumed cleanly; no further worker traffic queued.
    assert_no_event(AO_Video);
    assert_no_event(AO_USBAudio);
    assert_no_event(AO_Subtitle);
    assert_no_event(AO_Stt);
}
