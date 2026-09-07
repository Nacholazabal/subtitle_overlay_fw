/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file SttAO.c
/// @brief Speech-to-text input active object
///

// === Headers files inclusions ==================================================================================== //

#include "SttAO.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include "qpc.h"

#include "app.h"
#include "log.h"
#include "stt_ws_client.h"
#include "stt_ws_config.h"
#include "SubtitleAO.h"

// === Macros definitions ========================================================================================== //

#define STT_AO_POLL_TICKS           (1U)
#define STT_AO_POLL_PERIOD_MS       (10U)
/// One metrics line every 5 s at a 10 ms poll: with the PC bridge gone this
/// log is the only place the run's counters are recorded.
#define STT_AO_METRICS_POLLS        (500U)
#if CONFIG_TRACE_ENABLED
/// Profiling counters are sampled once per second, not once per operation.
    #define STT_AO_COUNTER_POLLS (100U)
#endif
#define STT_AO_MAX_EVENTS_PER_POLL  (4U)
#define STT_AO_PARTIAL_EVENT_MARGIN (4U)
#define STT_AO_FINAL_EVENT_MARGIN   (1U)

// === Private data type declarations ============================================================================== //

typedef struct
{
    QActive super;
    QTimeEvt poll_time_evt;

    stt_ws_client_t client_instance;  // Owned instance (no more singleton)
    stt_ws_client_t* client;          // Pointer to client_instance
    uint32_t polls_since_metrics;
    uint32_t polls_since_counters;
    uint8_t running;
} stt_ao_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static QState stt_ao_initial(stt_ao_t* const me, void const* const par);
static QState stt_ao_top(stt_ao_t* const me, QEvt const* const e);
static QState stt_ao_idle(stt_ao_t* const me, QEvt const* const e);
static QState stt_ao_ready(stt_ao_t* const me, QEvt const* const e);
static QState stt_ao_error(stt_ao_t* const me, QEvt const* const e);
static QState stt_ao_stopping(stt_ao_t* const me, QEvt const* const e);
static QState stt_ao_stopped(stt_ao_t* const me, QEvt const* const e);

static void post_ready(stt_ao_t* const me);
static void post_error(stt_ao_t* const me, int32_t code);
static void post_stopped(stt_ao_t* const me);
static int on_component_init(stt_ao_t* const me);
static int on_poll(stt_ao_t* const me);
static int on_transcript(stt_ao_t* const me, subtitle_text_evt_t const* const e);
static void emit_health_counters(stt_ao_t* me);
#if CONFIG_TRACE_ENABLED
static uint32_t visible_char_count(char const* text);
static void trace_dispatch(subtitle_text_evt_t const* e, char const* outcome);
#else
/* A production build must not carry the helpers, only consume the arguments. */
    #define trace_dispatch(event_, outcome_)                                                       \
        do                                                                                         \
        {                                                                                          \
            (void)(event_);                                                                        \
            (void)(outcome_);                                                                      \
        } while (0)
#endif
static void log_metrics(stt_ao_t* const me);
static void begin_stop(stt_ao_t* const me);
static void complete_stop(stt_ao_t* const me);
static void enter_error(stt_ao_t* const me, int32_t code);

// === Private variable definitions ================================================================================ //

static stt_ao_t stt_ao_inst;

// === Public variable definitions ================================================================================= //

QActive* const AO_Stt = Q_ACTIVE_UPCAST(&stt_ao_inst);

// === Private function implementation ============================================================================= //

/**
 * @brief Post a component-ready report to system_ao_t.
 * @param me STT active object sending the report.
 * @return None.
 */
static void post_ready(stt_ao_t* const me)
{
    component_ready_evt_t* const ready_evt =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, COMPONENT_READY_SIG);

    Q_UNUSED_PAR(me);

    if (ready_evt == NULL)
    {
        LOG_ERROR("stt: failed to allocate ready event");
        return;
    }

    ready_evt->source = COMPONENT_STT;
    ready_evt->width = 0U;
    ready_evt->height = 0U;
    if (!QACTIVE_POST_X(AO_System, &ready_evt->super, APP_CONTROL_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("stt: failed to post ready event");
    }
}

/**
 * @brief Post a component-error report to system_ao_t.
 * @param me STT active object sending the report.
 * @param code Negative errno-style value to include in the report.
 * @return None.
 */
static void post_error(stt_ao_t* const me, int32_t code)
{
    app_error_evt_t* const error_evt =
        Q_NEW_X(app_error_evt_t, APP_ERROR_EVENT_MARGIN, COMPONENT_ERROR_SIG);

    Q_UNUSED_PAR(me);

    if (error_evt == NULL)
    {
        LOG_ERROR("stt: failed to allocate error event, code=%ld", (long)code);
        return;
    }

    error_evt->source = COMPONENT_STT;
    error_evt->code = code;
    if (!QACTIVE_POST_X(AO_System, &error_evt->super, APP_ERROR_EVENT_MARGIN, &me->super))
    {
        LOG_ERROR("stt: failed to post error event, code=%ld", (long)code);
    }
}

/**
 * @brief Start the STT transcript TCP receiver.
 * @param me STT active object.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int on_component_init(stt_ao_t* const me)
{
    stt_ws_config_t config;
    int status;

    // Initialize the client owned by this AO (no more singleton)
    if (stt_ws_config_default(&config) != 0)
    {
        LOG_ERROR("stt: configuration failed (SUBTITLE_STT_WS_URL not set?)");
        enter_error(me, -EINVAL);
        return -EINVAL;
    }
    if (stt_ws_config_parse_url(config.url, &config) != 0)
    {
        LOG_ERROR("stt: URL parsing failed");
        enter_error(me, -EINVAL);
        return -EINVAL;
    }

    status = stt_ws_client_init(&me->client_instance, &config);
    if (status != 0)
    {
        LOG_ERROR("stt: client init failed: %d", status);
        enter_error(me, status);
        return status;
    }

    me->client = &me->client_instance;
    stt_ws_client_set_active(me->client);  // Register for USB audio handoff

    if (stt_ws_client_start(me->client) != 0)
    {
        LOG_ERROR("stt: failed to start the STT network worker");
        me->client = NULL;
        enter_error(me, -EIO);
        return -EIO;
    }

    me->running = 1U;
    me->polls_since_metrics = 0U;
    QTimeEvt_armX(&me->poll_time_evt, STT_AO_POLL_TICKS, STT_AO_POLL_TICKS);
    post_ready(me);
    LOG_INFO("stt: transcript intake ready, poll period=%lu ms",
             (unsigned long)STT_AO_POLL_PERIOD_MS);

    return 0;
}

/**
 * @brief Poll the STT receiver and forward parsed transcript events.
 * @param me STT active object.
 * @return 0 on success, or a negative errno-style value on failure.
 */
static int on_poll(stt_ao_t* const me)
{
    subtitle_text_evt_t events[STT_AO_MAX_EVENTS_PER_POLL];
    uint32_t event_count = 0U;
    uint32_t i;
    int status;

    // Strictly nonblocking: this runs on the single QP/C thread, so any wait
    // here would also freeze video and subtitle rendering.
    status = stt_ws_client_poll_events(me->client, events, Q_DIM(events), &event_count);
    if (status != 0)
    {
        LOG_ERROR("stt: transcript poll failed, code=%ld", (long)status);
        return status;
    }

    for (i = 0U; i < event_count; i++)
    {
        (void)on_transcript(me, &events[i]);
    }

    log_metrics(me);
    emit_health_counters(me);
    return 0;
}

/**
 * @brief Sample queue depths, drop counters and process CPU once per second.
 *
 * Counters answer "did the queues or the CPU fail to keep up?" without emitting
 * an event per operation, which is what would distort the measurement.
 */
static void emit_health_counters(stt_ao_t* const me)
{
#if CONFIG_TRACE_ENABLED
    stt_ws_client_stats_t stats;
    uint32_t audio_depth = 0U;
    uint32_t event_depth = 0U;

    me->polls_since_counters++;
    if (me->polls_since_counters < STT_AO_COUNTER_POLLS)
    {
        return;
    }
    me->polls_since_counters = 0U;

    stt_ws_client_stats(me->client, &stats);
    stt_ws_client_queue_depths(me->client, &audio_depth, &event_depth);

    TRACE_COUNTER(g_trace,
                  "pipeline_health",
                  TRACE_U64("audio_txq_depth", audio_depth),
                  TRACE_U64("event_ring_depth", event_depth),
                  TRACE_U64("chunks_sent", stats.chunks_sent),
                  TRACE_U64("chunks_dropped_tx", stats.chunks_dropped_tx),
                  TRACE_U64("transcripts_final", stats.transcripts_final),
                  TRACE_U64("transcripts_partial", stats.transcripts_partial),
                  TRACE_U64("deliveries_accepted", stats.deliveries_accepted),
                  TRACE_U64("dropped_pool", stats.deliveries_dropped_pool),
                  TRACE_U64("dropped_queue", stats.deliveries_dropped_queue),
                  TRACE_U64("dropped_ring", stats.events_dropped_ring),
                  TRACE_U64("reconnects", stats.reconnects),
                  TRACE_U64("protocol_errors", stats.protocol_errors));

    {
        struct rusage usage;

        if (getrusage(RUSAGE_SELF, &usage) == 0)
        {
            TRACE_COUNTER(g_trace,
                          "process_health",
                          TRACE_F64("user_cpu_sec", (double)usage.ru_utime.tv_sec
                                                        + ((double)usage.ru_utime.tv_usec / 1.0e6)),
                          TRACE_F64("system_cpu_sec",
                                    (double)usage.ru_stime.tv_sec
                                        + ((double)usage.ru_stime.tv_usec / 1.0e6)),
                          TRACE_U64("max_rss_kib", (uint64_t)usage.ru_maxrss),
                          TRACE_U64("ctx_switches_voluntary", (uint64_t)usage.ru_nvcsw),
                          TRACE_U64("ctx_switches_forced", (uint64_t)usage.ru_nivcsw));
        }
    }
#else
    (void)me;
#endif
}

/** @brief Emit the periodic counters line that replaces the PC bridge report. */
static void log_metrics(stt_ao_t* const me)
{
    stt_ws_client_stats_t stats;

    me->polls_since_metrics++;
    if (me->polls_since_metrics < STT_AO_METRICS_POLLS)
    {
        return;
    }
    me->polls_since_metrics = 0U;

    stt_ws_client_stats(me->client, &stats);
    LOG_INFO("stt: link=%s sessions=%lu reconnects=%lu chunks sent=%lu dropped=%lu",
             stt_ws_client_state_name(stt_ws_client_state(me->client)),
             (unsigned long)stats.sessions,
             (unsigned long)stats.reconnects,
             (unsigned long)stats.chunks_sent,
             (unsigned long)stats.chunks_dropped_tx);
    LOG_INFO("stt: transcripts=%lu (final=%lu partial=%lu) delivered=%lu "
             "dropped(pool=%lu queue=%lu ring=%lu) stale=%lu proto_err=%lu",
             (unsigned long)stats.transcripts_received,
             (unsigned long)stats.transcripts_final,
             (unsigned long)stats.transcripts_partial,
             (unsigned long)stats.deliveries_accepted,
             (unsigned long)stats.deliveries_dropped_pool,
             (unsigned long)stats.deliveries_dropped_queue,
             (unsigned long)stats.events_dropped_ring,
             (unsigned long)stats.events_rejected_old_seq,
             (unsigned long)stats.protocol_errors);
    if ((stats.clock_deferrals > 0U) || (stats.tls_failures > 0U) || (stats.busy_rejections > 0U))
    {
        LOG_INFO("stt: clock_deferrals=%lu tls_failures=%lu busy=%lu connect_failures=%lu",
                 (unsigned long)stats.clock_deferrals,
                 (unsigned long)stats.tls_failures,
                 (unsigned long)stats.busy_rejections,
                 (unsigned long)stats.connect_failures);
    }
}

#if CONFIG_TRACE_ENABLED
/** @brief Count UTF-8 code points, i.e. what a viewer perceives as characters. */
static uint32_t visible_char_count(char const* const text)
{
    uint32_t count = 0U;
    size_t index;

    for (index = 0U; text[index] != '\0'; index++)
    {
        // Continuation bytes extend the previous code point.
        if (((uint8_t)text[index] & 0xC0U) != 0x80U)
        {
            count++;
        }
    }

    return count;
}

/**
 * @brief Record the outcome of one transcript hand-off to SubtitleAO.
 *
 * The transcript text itself never reaches the trace: sizes and the sequence
 * number are enough to reconcile the pipeline, and keeping text out avoids
 * putting speech content in a profiling artifact.
 */
static void trace_dispatch(subtitle_text_evt_t const* const e, char const* const outcome)
{
    TRACE_INSTANT(g_trace,
                  "transcript_dispatch",
                  TRACE_U64("transcript_seq", e->seq),
                  TRACE_BOOL("is_final", e->is_final),
                  TRACE_U64("text_bytes", (uint64_t)strlen(e->text)),
                  TRACE_U64("visible_chars", visible_char_count(e->text)),
                  TRACE_U64("start_ms", e->start_ms),
                  TRACE_U64("end_ms", e->end_ms),
                  TRACE_STR("outcome", outcome));
}
#endif /* CONFIG_TRACE_ENABLED */

/**
 * @brief Forward a valid transcript event to the subtitle active object.
 * @param me STT active object.
 * @param e Transcript event.
 * @return 0 on success, -EAGAIN when discarded.
 */
static int on_transcript(stt_ao_t* const me, subtitle_text_evt_t const* const e)
{
    subtitle_text_evt_t* subtitle_evt;
    uint16_t margin;

    // Validate me/e before reading any field.
    if ((me == NULL) || (e == NULL) || (e->text[0] == '\0'))
    {
        return -EINVAL;
    }

    margin = (e->is_final != 0U) ? STT_AO_FINAL_EVENT_MARGIN : STT_AO_PARTIAL_EVENT_MARGIN;

    subtitle_evt = Q_NEW_X(subtitle_text_evt_t, margin, SUBTITLE_TEXT_SIG);
    if (subtitle_evt == NULL)
    {
        LOG_WARNING("stt: dropping %s transcript seq=%lu, event pool margin=%u",
                    (e->is_final != 0U) ? "final" : "partial",
                    (unsigned long)e->seq,
                    (unsigned)margin);
        stt_ws_client_report_delivery(me->client, STT_TRANSCRIPT_DELIVERY_DROPPED_EVENT_POOL);
        trace_dispatch(e, "dropped_event_pool");
        return -EAGAIN;
    }

    subtitle_evt->seq = e->seq;
    subtitle_evt->start_ms = e->start_ms;
    subtitle_evt->end_ms = e->end_ms;
    subtitle_evt->is_final = e->is_final;
    snprintf(subtitle_evt->text, sizeof(subtitle_evt->text), "%s", e->text);

    // DEBUG: runs per transcript; the metrics line already reports volume.
    LOG_DEBUG("stt: forwarding %s transcript seq=%lu",
              (e->is_final != 0U) ? "final" : "partial",
              (unsigned long)e->seq);
    if (!QACTIVE_POST_X(AO_Subtitle, &subtitle_evt->super, margin, &me->super))
    {
        LOG_WARNING("stt: dropping %s transcript seq=%lu, subtitle queue margin=%u",
                    (e->is_final != 0U) ? "final" : "partial",
                    (unsigned long)e->seq,
                    (unsigned)margin);
        stt_ws_client_report_delivery(me->client, STT_TRANSCRIPT_DELIVERY_DROPPED_SUBTITLE_QUEUE);
        trace_dispatch(e, "dropped_subtitle_queue");
        return -EAGAIN;
    }

    trace_dispatch(e, "accepted");

    // "Delivered" now means "posted to the subtitle AO": with the PC bridge
    // gone there is no peer to acknowledge to, so the outcome is a counter.
    stt_ws_client_report_delivery(me->client, STT_TRANSCRIPT_DELIVERY_ACCEPTED);

    return 0;
}

/**
 * @brief Stop the STT receiver and report a terminal error.
 * @param me STT active object entering error.
 * @param code Negative errno-style value.
 * @return None.
 */
// Request-only phase: it runs in a QP/C handler and therefore cannot join a
// network thread that may still be inside a bounded DNS/TLS operation.
static void begin_stop(stt_ao_t* const me)
{
    if (me->client != NULL)
    {
        stt_ws_client_request_stop(me->client);
    }
}

// Completion phase: finish_stop only joins after stop_complete proves that the
// worker has exited, so this remains bounded on the cooperative QP/C thread.
static void complete_stop(stt_ao_t* const me)
{
    (void)QTimeEvt_disarm(&me->poll_time_evt);
    if (me->client != NULL)
    {
        (void)stt_ws_client_finish_stop(me->client);
        me->client = NULL;
    }
    me->running = 0U;
}

// Acknowledge SYSTEM_STOP to system_ao_t once this AO has quiesced.
static void post_stopped(stt_ao_t* const me)
{
    Q_UNUSED_PAR(me); // used only as the QS trace sender (dropped without Q_SPY)

    component_ready_evt_t* const evt =
        Q_NEW_X(component_ready_evt_t, APP_CONTROL_EVENT_MARGIN, SYSTEM_STOPPED_SIG);

    if (evt != NULL)
    {
        evt->source = COMPONENT_STT;
        evt->width = 0U;
        evt->height = 0U;
        (void)QACTIVE_POST_X(AO_System, &evt->super, APP_CONTROL_EVENT_MARGIN, &me->super);
    }
}

static void enter_error(stt_ao_t* const me, int32_t code)
{
    begin_stop(me);
    post_error(me, code);
}

/**
 * @brief Run the initial transition for stt_ao_t.
 * @param me STT active object instance.
 * @param par Optional initial transition parameter.
 * @return QP/C transition result.
 */
static QState stt_ao_initial(stt_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(par);

    return Q_TRAN(&stt_ao_idle);
}

/**
 * @brief Superstate handling the coordinated SYSTEM_STOP command in any substate.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_top(stt_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case SYSTEM_STOP_SIG:
        begin_stop(me);
        if ((me->client == NULL) || (stt_ws_client_stop_complete(me->client) != 0U))
        {
            complete_stop(me);
            post_stopped(me);
            status = Q_TRAN(&stt_ao_stopped);
        }
        else
        {
            status = Q_TRAN(&stt_ao_stopping);
        }
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

/**
 * @brief Handle initialization before the STT receiver is running.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_idle(stt_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case COMPONENT_INIT_SIG:
        if (on_component_init(me) == 0)
        {
            status = Q_TRAN(&stt_ao_ready);
        }
        else
        {
            status = Q_TRAN(&stt_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&stt_ao_top);
        break;
    }

    return status;
}

/**
 * @brief Hold the running STT receiver and forward transcript events.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_ready(stt_ao_t* const me, QEvt const* const e)
{
    QState status;
    int poll_status;

    switch (e->sig)
    {
    case STT_POLL_SIG:
        poll_status = on_poll(me);
        if (poll_status == 0)
        {
            status = Q_HANDLED();
        }
        else
        {
            enter_error(me, poll_status);
            status = Q_TRAN(&stt_ao_error);
        }
        break;

    default:
        status = Q_SUPER(&stt_ao_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state reached after STT receiver initialization fails.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_error(stt_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);
    Q_UNUSED_PAR(e);

    return Q_SUPER(&stt_ao_top);
}

/** @brief Poll an asynchronously stopping network worker without doing I/O. */
static QState stt_ao_stopping(stt_ao_t* const me, QEvt const* const e)
{
    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        LOG_INFO("stt: waiting for network worker shutdown");
        status = Q_HANDLED();
        break;

    case STT_POLL_SIG:
        if ((me->client == NULL) || (stt_ws_client_stop_complete(me->client) != 0U))
        {
            complete_stop(me);
            post_stopped(me);
            status = Q_TRAN(&stt_ao_stopped);
        }
        else
        {
            status = Q_HANDLED();
        }
        break;

    case SYSTEM_STOP_SIG:
        status = Q_HANDLED();
        break;

    default:
        status = Q_SUPER(&stt_ao_top);
        break;
    }

    return status;
}

/**
 * @brief Terminal state after a coordinated SYSTEM_STOP: the receiver is closed.
 * @param me STT active object instance.
 * @param e Event dispatched by QP/C.
 * @return QP/C state handler result.
 */
static QState stt_ao_stopped(stt_ao_t* const me, QEvt const* const e)
{
    Q_UNUSED_PAR(me);

    QState status;

    switch (e->sig)
    {
    case Q_ENTRY_SIG:
        LOG_INFO("stt: stopped");
        status = Q_HANDLED();
        break;

    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }

    return status;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Construct the STT active object.
 * @param None.
 * @return None.
 */
void stt_ao_ctor(void)
{
    stt_ao_t* const me = &stt_ao_inst;

    QActive_ctor(&me->super, Q_STATE_CAST(&stt_ao_initial));
    QTimeEvt_ctorX(&me->poll_time_evt, &me->super, STT_POLL_SIG, 0U);
    me->client = NULL;
    me->running = 0U;
}

// === End of documentation ======================================================================================== //
