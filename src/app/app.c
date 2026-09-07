/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file app.c
/// @brief QP/C POSIX startup and active-object wiring
///

// === Headers files inclusions ==================================================================================== //

#include "app.h"

#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "trace.h"
#include "SttAO.h"
#include "SubtitleAO.h"
#include "SystemAO.h"
#include "USBAudioAO.h"
#include "VideoAO.h"

// === Macros definitions ========================================================================================== //

#define APP_TICKS_PER_SEC      (100U)
#define APP_EVENT_POOL_LEN     (64U)
#define SYSTEM_AO_QUEUE_LEN    (8U)
#define SUBTITLE_AO_QUEUE_LEN  (16U)
#define STT_AO_QUEUE_LEN       (8U)
#define USB_AUDIO_AO_QUEUE_LEN (8U)
#define VIDEO_AO_QUEUE_LEN     (16U)

// AO priorities follow rate-monotonic scheduling: shorter period = higher priority.
// STT (10 ms) has the tightest deadline; Subtitle is event-driven with the loosest.
#define SYSTEM_AO_PRIO    (1U) // Orchestration; lowest priority.
#define SUBTITLE_AO_PRIO  (2U) // Event-driven; loosest deadline.
#define VIDEO_AO_PRIO     (3U) // 100 ms poll.
#define USB_AUDIO_AO_PRIO (4U) // 100 ms poll; same period as video.
#define STT_AO_PRIO       (5U) // 10 ms poll; tightest deadline, highest priority.

// === Private data type declarations ============================================================================== //

typedef union
{
    component_init_evt_t component_init;
    component_ready_evt_t component_ready;
    app_error_evt_t app_error;
    subtitle_text_evt_t subtitle_text;
} app_event_pool_evt_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void bsp_init_placeholder(void);
static void app_init(void);
static void app_log_output(log_level_e severity, const char* msg);
static void app_install_signal_handlers(void);
static void app_shutdown_signal_handler(int signal_number);

// === Public variable definitions ================================================================================= //

/// Global trace context for performance profiling
trace_ctx_t* g_trace = NULL;

// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

static void bsp_init_placeholder(void)
{
    /*
     * TODO: Replace this placeholder with BSP_init() when the Linux BSP exists.
     * Keep board/platform initialization behind bsp/ and hardware access behind
     * hal/. app.c should remain focused on QP/C startup and AO wiring.
     */
}

// Called from the QP/C thread and both workers, so it must not block. stdout is
// line buffered; only errors are worth forcing out immediately.
static void app_log_output(log_level_e severity, const char* msg)
{
    fprintf(stdout, "[%s] %s\n", log_level_to_str(severity), msg);
    if (severity >= LOG_LEVEL_ERROR)
    {
        fflush(stdout);
    }
}

// Signal handlers may only perform async-signal-safe work. The ticker thread
// translates this flag into a normal QP/C event on its next 10 ms tick.
static volatile sig_atomic_t shutdown_signal_pending;
static volatile sig_atomic_t shutdown_event_posted;

static void app_shutdown_signal_handler(int const signal_number)
{
    Q_UNUSED_PAR(signal_number);
    shutdown_signal_pending = 1;
}

static void app_install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = &app_shutdown_signal_handler;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    // OpenSSL's SSL_write() ultimately writes to a socket. Ignore SIGPIPE so a
    // peer reset is reported as an I/O error and reaches the reconnect path.
    (void)signal(SIGPIPE, SIG_IGN);
}

// === Public function implementation ============================================================================== //

static void app_init(void)
{
    LOG_INFO("app: initializing QP/C event pools and active objects");

    static QF_MPOOL_EL(app_event_pool_evt_t) app_event_pool_sto[APP_EVENT_POOL_LEN];
    /* Partial subtitles are intentionally shed first through their larger Q_NEW_X margin;
     * control/final/error traffic retains reserved pool capacity during transcript bursts. */
    QF_poolInit(app_event_pool_sto, sizeof(app_event_pool_sto), sizeof(app_event_pool_sto[0]));

    static QEvtPtr video_queue_sto[VIDEO_AO_QUEUE_LEN];
    video_ao_ctor();
    QActive_start(AO_Video,
                  VIDEO_AO_PRIO,
                  video_queue_sto,
                  Q_DIM(video_queue_sto),
                  (void*)0,
                  0U,
                  (void*)0);

    static QEvtPtr usb_audio_queue_sto[USB_AUDIO_AO_QUEUE_LEN];
    usb_audio_ao_ctor();
    QActive_start(AO_USBAudio,
                  USB_AUDIO_AO_PRIO,
                  usb_audio_queue_sto,
                  Q_DIM(usb_audio_queue_sto),
                  (void*)0,
                  0U,
                  (void*)0);

    static QEvtPtr subtitle_queue_sto[SUBTITLE_AO_QUEUE_LEN];
    subtitle_ao_ctor();
    QActive_start(AO_Subtitle,
                  SUBTITLE_AO_PRIO,
                  subtitle_queue_sto,
                  Q_DIM(subtitle_queue_sto),
                  (void*)0,
                  0U,
                  (void*)0);

    static QEvtPtr stt_queue_sto[STT_AO_QUEUE_LEN];
    stt_ao_ctor();
    QActive_start(AO_Stt, STT_AO_PRIO, stt_queue_sto, Q_DIM(stt_queue_sto), (void*)0, 0U, (void*)0);

    // SystemAO must start last: QActive_start() runs the initial transition immediately,
    // and system_ao_init's Q_ENTRY_SIG posts COMPONENT_INIT to the other AOs. Starting
    // SystemAO earlier would post to unregistered active objects and trip a QP/C assert.
    static QEvtPtr system_queue_sto[SYSTEM_AO_QUEUE_LEN];
    system_ao_ctor();
    QActive_start(AO_System,
                  SYSTEM_AO_PRIO,
                  system_queue_sto,
                  Q_DIM(system_queue_sto),
                  (void*)0,
                  0U,
                  (void*)0);

    LOG_INFO("app: active objects started");

    /*
     * TODO: Construct and start ButtonsAO and LEDAO here as they are
     * implemented.
     */
}

int main(void)
{
    // One write per log record, even when stdout is a pipe.
    (void)setvbuf(stdout, NULL, _IOLBF, 0);

    log_init();
    (void)log_subscribe(app_log_output, LOG_LEVEL_INFO);
    LOG_INFO("app: starting subtitle overlay firmware");

#if CONFIG_TRACE_ENABLED
    // Profiling builds emit firmware events to /tmp/fw_trace.jsonl.
    g_trace = trace_init(NULL);
    if (!g_trace)
    {
        LOG_WARNING("app: tracing disabled (failed to init)");
    }
#endif

    QF_init();
    app_install_signal_handlers();
    bsp_init_placeholder();
    app_init();

    int const ret = QF_run();

#if CONFIG_TRACE_ENABLED
    trace_close(g_trace);
#endif
    return ret;
}

// === QP/C POSIX callbacks ======================================================================================== //

Q_NORETURN Q_onError(char const* const module, int_t const id)
{
    fprintf(stderr, "QP/C assertion failed in %s:%d\n", module, id);
    exit(EXIT_FAILURE);
}

void QF_onStartup(void)
{
    QF_setTickRate(APP_TICKS_PER_SEC, 0);
}

void QF_onCleanup(void)
{}

void QF_onClockTick(void)
{
    static QEvt const shutdown_evt = QEVT_INITIALIZER(SYSTEM_SHUTDOWN_SIG);

    QTIMEEVT_TICK_X(0U, (void*)0);
    if ((shutdown_signal_pending != 0) && (shutdown_event_posted == 0))
    {
        shutdown_event_posted = 1;
        (void)QACTIVE_POST_X(AO_System, &shutdown_evt, 0U, (void*)0);
    }
}

// === End of documentation ======================================================================================== //
