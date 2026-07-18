#include "unity.h"

#include "app.h"
#include "mock_stt_event_rx.h"
#include "mock_subtitle_pipeline.h"
#include "qpc_test_harness.h"
#include "SttAO.h"
#include "SubtitleAO.h"

TEST_SOURCE_FILE("qpc_test_harness.c")
TEST_SOURCE_FILE("SttAO.c")
TEST_SOURCE_FILE("SubtitleAO.c")
TEST_SOURCE_FILE("log.c")
TEST_SOURCE_FILE("number_parse.c")
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
static QEvtPtr system_queue[16];
static QEvtPtr stt_queue[16];
static QEvtPtr subtitle_queue[16];

QActive* const AO_System = &system_fake.super;

static char captured_text[SUBTITLE_TEXT_MAX_LEN * 2U];
static uint8_t captured_current_is_final;
static int write_text_status;
static subtitle_text_evt_t poll_events[STT_EVENT_RX_MAX_EVENTS_PER_POLL];
static uint32_t poll_event_count;
static int poll_status;

static void start_system_fake(void)
{
    qpc_test_fake_ao_ctor(&system_fake);
    qpc_test_start(AO_System, 1U, system_queue, Q_DIM(system_queue));
}

static void start_stt_ao(void)
{
    stt_ao_ctor();
    qpc_test_start(AO_Stt, 2U, stt_queue, Q_DIM(stt_queue));
}

static void start_subtitle_ao(void)
{
    subtitle_ao_ctor();
    qpc_test_start(AO_Subtitle, 3U, subtitle_queue, Q_DIM(subtitle_queue));
}

static void expect_stt_init_success(void)
{
    stt_event_rx_default_config_Ignore();
    stt_event_rx_init_IgnoreAndReturn(0);
    stt_event_rx_report_delivery_IgnoreAndReturn(0);
}

static void post_component_init(QActive* const target, uint32_t width, uint32_t height)
{
    component_init_evt_t* const event =
        Q_NEW_X(component_init_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_INIT_SIG);

    event->source = COMPONENT_NONE;
    event->width = width;
    event->height = height;
    (void)QACTIVE_POST_X(target, &event->super, APP_CONTROL_EVENT_MARGIN, (void*)0);
}

static int subtitle_pipeline_write_caption_capture(subtitle_pipeline_t* pipeline,
                                                   char const* text,
                                                   uint8_t current_is_final,
                                                   int call_count)
{
    Q_UNUSED_PAR(pipeline);
    Q_UNUSED_PAR(call_count);

    snprintf(captured_text, sizeof(captured_text), "%s", text);
    captured_current_is_final = current_is_final;
    return write_text_status;
}

static int stt_event_rx_poll_stub(stt_event_rx_t* rx,
                                  subtitle_text_evt_t* events,
                                  uint32_t max_events,
                                  uint32_t* event_count,
                                  int call_count)
{
    uint32_t i;

    Q_UNUSED_PAR(rx);
    Q_UNUSED_PAR(call_count);

    if (poll_status == 0)
    {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(STT_EVENT_RX_MAX_EVENTS_PER_POLL, max_events);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(max_events, poll_event_count);
        for (i = 0U; i < poll_event_count; i++)
        {
            events[i] = poll_events[i];
        }
        *event_count = poll_event_count;
    }

    return poll_status;
}

static void set_stt_poll_result(subtitle_text_evt_t const* events, uint32_t count, int status)
{
    uint32_t i;

    memset(poll_events, 0, sizeof(poll_events));
    for (i = 0U; i < count; i++)
    {
        poll_events[i] = events[i];
    }
    poll_event_count = count;
    poll_status = status;
    stt_event_rx_poll_Stub(stt_event_rx_poll_stub);
}

static void expect_subtitle_init_success(void)
{
    subtitle_pipeline_init_IgnoreAndReturn(0);
    subtitle_pipeline_clear_IgnoreAndReturn(0);
    subtitle_pipeline_write_bitmap_IgnoreAndReturn(0);
    subtitle_pipeline_enable_IgnoreAndReturn(0);
}

static void init_subtitle_ready(void)
{
    expect_subtitle_init_success();
    start_subtitle_ao();

    post_component_init(AO_Subtitle, 1280U, 720U);
    qpc_test_dispatch_one(AO_Subtitle);
    qpc_test_gc(qpc_test_pop(AO_System));
}

static void expect_subtitle_render_success(void)
{
    write_text_status = 0;
    captured_text[0] = '\0';
    captured_current_is_final = 0U;
    subtitle_pipeline_clear_IgnoreAndReturn(0);
    subtitle_pipeline_write_caption_Stub(subtitle_pipeline_write_caption_capture);
    subtitle_pipeline_enable_IgnoreAndReturn(0);
}

static component_ready_evt_t const* pop_ready_from_system(void)
{
    QEvt const* const event = qpc_test_pop(AO_System);

    TEST_ASSERT_EQUAL_UINT(COMPONENT_READY_SIG, event->sig);
    return (component_ready_evt_t const*)event;
}

static app_error_evt_t const* pop_error_from_system(void)
{
    QEvt const* const event = qpc_test_pop(AO_System);

    TEST_ASSERT_EQUAL_UINT(COMPONENT_ERROR_SIG, event->sig);
    return (app_error_evt_t const*)event;
}

static void assert_no_system_event(void)
{
    TEST_ASSERT_EQUAL_UINT16(0U, qpc_test_queue_use(AO_System));
}

void setUp(void)
{
    qpc_test_init();
    start_system_fake();
    write_text_status = 0;
    captured_text[0] = '\0';
    captured_current_is_final = 0U;
    memset(poll_events, 0, sizeof(poll_events));
    poll_event_count = 0U;
    poll_status = 0;
}

void tearDown(void)
{}

void test_stt_init_success_posts_ready_to_system(void)
{
    component_ready_evt_t const* ready;

    expect_stt_init_success();
    start_stt_ao();

    post_component_init(AO_Stt, 0U, 0U);
    qpc_test_dispatch_one(AO_Stt);

    ready = pop_ready_from_system();
    TEST_ASSERT_EQUAL_INT(COMPONENT_STT, ready->source);
    TEST_ASSERT_EQUAL_UINT32(0U, ready->width);
    TEST_ASSERT_EQUAL_UINT32(0U, ready->height);
    qpc_test_gc(&ready->super);
}

void test_subtitle_init_success_draws_marker_and_posts_ready_to_system(void)
{
    component_ready_evt_t const* ready;

    expect_subtitle_init_success();
    start_subtitle_ao();

    post_component_init(AO_Subtitle, 1280U, 720U);
    qpc_test_dispatch_one(AO_Subtitle);

    ready = pop_ready_from_system();
    TEST_ASSERT_EQUAL_INT(COMPONENT_SUBTITLE_PIPELINE, ready->source);
    qpc_test_gc(&ready->super);
}

void test_stt_poll_partial_and_final_render_as_current_segment(void)
{
    subtitle_text_evt_t events[1];

    expect_stt_init_success();
    expect_subtitle_init_success();
    start_stt_ao();
    start_subtitle_ao();

    post_component_init(AO_Stt, 0U, 0U);
    qpc_test_dispatch_one(AO_Stt);
    qpc_test_gc(qpc_test_pop(AO_System));

    post_component_init(AO_Subtitle, 1280U, 720U);
    qpc_test_dispatch_one(AO_Subtitle);
    qpc_test_gc(qpc_test_pop(AO_System));

    memset(events, 0, sizeof(events));
    events[0].seq = 1U;
    events[0].is_final = 0U;
    snprintf(events[0].text, sizeof(events[0].text), "%s", "hola parcial");
    set_stt_poll_result(events, 1U, 0);

    qpc_test_post_signal(AO_Stt, STT_POLL_SIG);
    qpc_test_dispatch_one(AO_Stt);

    expect_subtitle_render_success();
    qpc_test_dispatch_one(AO_Subtitle);
    TEST_ASSERT_EQUAL_STRING("hola parcial", captured_text);
    TEST_ASSERT_EQUAL_UINT8(0U, captured_current_is_final);

    memset(events, 0, sizeof(events));
    events[0].seq = 2U;
    events[0].is_final = 1U;
    snprintf(events[0].text, sizeof(events[0].text), "%s", "final");
    set_stt_poll_result(events, 1U, 0);

    qpc_test_post_signal(AO_Stt, STT_POLL_SIG);
    qpc_test_dispatch_one(AO_Stt);

    expect_subtitle_render_success();
    qpc_test_dispatch_one(AO_Subtitle);
    TEST_ASSERT_EQUAL_STRING("final", captured_text);
    TEST_ASSERT_EQUAL_UINT8(1U, captured_current_is_final);
}

void test_subtitle_broadcast_promotes_final_when_new_partial_arrives(void)
{
    init_subtitle_ready();

    expect_subtitle_render_success();
    qpc_test_post_subtitle_text(AO_Subtitle, 1U, 1U, "segmento final");
    qpc_test_dispatch_one(AO_Subtitle);
    TEST_ASSERT_EQUAL_STRING("segmento final", captured_text);
    TEST_ASSERT_EQUAL_UINT8(1U, captured_current_is_final);

    expect_subtitle_render_success();
    qpc_test_post_subtitle_text(AO_Subtitle, 2U, 0U, "parcial nuevo");
    qpc_test_dispatch_one(AO_Subtitle);
    TEST_ASSERT_EQUAL_STRING("segmento final\nparcial nuevo", captured_text);
    TEST_ASSERT_EQUAL_UINT8(0U, captured_current_is_final);
}

void test_subtitle_previous_expire_keeps_current_live_segment(void)
{
    init_subtitle_ready();

    expect_subtitle_render_success();
    qpc_test_post_subtitle_text(AO_Subtitle, 1U, 1U, "anterior");
    qpc_test_dispatch_one(AO_Subtitle);

    expect_subtitle_render_success();
    qpc_test_post_subtitle_text(AO_Subtitle, 2U, 0U, "actual");
    qpc_test_dispatch_one(AO_Subtitle);

    expect_subtitle_render_success();
    qpc_test_post_signal(AO_Subtitle, SUBTITLE_PREVIOUS_EXPIRE_SIG);
    qpc_test_dispatch_one(AO_Subtitle);
    TEST_ASSERT_EQUAL_STRING("actual", captured_text);
    TEST_ASSERT_EQUAL_UINT8(0U, captured_current_is_final);
}

void test_subtitle_clear_signal_clears_all_broadcast_slots(void)
{
    init_subtitle_ready();

    expect_subtitle_render_success();
    qpc_test_post_subtitle_text(AO_Subtitle, 1U, 1U, "anterior");
    qpc_test_dispatch_one(AO_Subtitle);

    expect_subtitle_render_success();
    qpc_test_post_subtitle_text(AO_Subtitle, 2U, 0U, "actual");
    qpc_test_dispatch_one(AO_Subtitle);

    subtitle_pipeline_clear_ExpectAnyArgsAndReturn(0);
    subtitle_pipeline_enable_ExpectAnyArgsAndReturn(0);
    qpc_test_post_signal(AO_Subtitle, SUBTITLE_CLEAR_SIG);
    qpc_test_dispatch_one(AO_Subtitle);

    expect_subtitle_render_success();
    qpc_test_post_subtitle_text(AO_Subtitle, 3U, 0U, "solo actual");
    qpc_test_dispatch_one(AO_Subtitle);
    TEST_ASSERT_EQUAL_STRING("solo actual", captured_text);
}

void test_stt_forwards_every_event_returned_by_session_aware_receiver(void)
{
    subtitle_text_evt_t events[2];

    expect_stt_init_success();
    start_stt_ao();

    post_component_init(AO_Stt, 0U, 0U);
    qpc_test_dispatch_one(AO_Stt);
    qpc_test_gc(qpc_test_pop(AO_System));

    memset(events, 0, sizeof(events));
    events[0].seq = 10U;
    events[0].is_final = 1U;
    snprintf(events[0].text, sizeof(events[0].text), "%s", "primero");
    events[1].seq = 10U;
    events[1].is_final = 1U;
    snprintf(events[1].text, sizeof(events[1].text), "%s", "duplicado");

    set_stt_poll_result(events, 2U, 0);

    qpc_test_post_signal(AO_Stt, STT_POLL_SIG);
    qpc_test_dispatch_one(AO_Stt);

    TEST_ASSERT_EQUAL_UINT16(2U, qpc_test_queue_use(AO_Subtitle));
    qpc_test_gc(qpc_test_pop(AO_Subtitle));
    qpc_test_gc(qpc_test_pop(AO_Subtitle));
}

void test_stt_poll_error_posts_component_error(void)
{
    app_error_evt_t const* error;

    expect_stt_init_success();
    start_stt_ao();

    post_component_init(AO_Stt, 0U, 0U);
    qpc_test_dispatch_one(AO_Stt);
    qpc_test_gc(qpc_test_pop(AO_System));

    set_stt_poll_result(NULL, 0U, -EIO);
    stt_event_rx_cleanup_Ignore();

    qpc_test_post_signal(AO_Stt, STT_POLL_SIG);
    qpc_test_dispatch_one(AO_Stt);

    error = pop_error_from_system();
    TEST_ASSERT_EQUAL_INT(COMPONENT_STT, error->source);
    TEST_ASSERT_EQUAL_INT(-EIO, error->code);
    qpc_test_gc(&error->super);
}

void test_subtitle_render_error_posts_component_error(void)
{
    app_error_evt_t const* error;

    expect_subtitle_init_success();
    start_subtitle_ao();

    post_component_init(AO_Subtitle, 1280U, 720U);
    qpc_test_dispatch_one(AO_Subtitle);
    qpc_test_gc(qpc_test_pop(AO_System));

    write_text_status = -EIO;
    subtitle_pipeline_clear_IgnoreAndReturn(0);
    subtitle_pipeline_write_caption_Stub(subtitle_pipeline_write_caption_capture);
    subtitle_pipeline_cleanup_Ignore();

    qpc_test_post_subtitle_text(AO_Subtitle, 1U, 1U, "rompe");
    qpc_test_dispatch_one(AO_Subtitle);

    error = pop_error_from_system();
    TEST_ASSERT_EQUAL_INT(COMPONENT_SUBTITLE_PIPELINE, error->source);
    TEST_ASSERT_EQUAL_INT(-EIO, error->code);
    qpc_test_gc(&error->super);
    assert_no_system_event();
}
