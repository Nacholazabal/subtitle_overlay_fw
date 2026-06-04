/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file app.c
/// @brief QP/C POSIX startup and active-object wiring
///

// === Headers files inclusions ==================================================================================== //

#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "SubtitleAO.h"
#include "SystemAO.h"
#include "VideoAO.h"

// === Macros definitions ========================================================================================== //

#define APP_TICKS_PER_SEC     (100U)
#define APP_ERROR_POOL_LEN    (8U)
#define APP_READY_POOL_LEN    (8U)
#define SYSTEM_AO_QUEUE_LEN   (8U)
#define SUBTITLE_AO_QUEUE_LEN (8U)
#define VIDEO_AO_QUEUE_LEN    (8U)
#define SYSTEM_AO_PRIO        (1U)
#define VIDEO_AO_PRIO         (2U)
#define SUBTITLE_AO_PRIO      (3U)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void bsp_init_placeholder(void);
static void app_init(void);

// === Public variable definitions ================================================================================= //
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

// === Public function implementation ============================================================================== //

static void app_init(void)
{
    /* QP/C event pools must be initialized in increasing event-size order. */
    static QF_MPOOL_EL(component_ready_evt_t) app_ready_pool_sto[APP_READY_POOL_LEN];
    QF_poolInit(app_ready_pool_sto, sizeof(app_ready_pool_sto), sizeof(app_ready_pool_sto[0]));

    static QF_MPOOL_EL(app_error_evt_t) app_error_pool_sto[APP_ERROR_POOL_LEN];
    QF_poolInit(app_error_pool_sto, sizeof(app_error_pool_sto), sizeof(app_error_pool_sto[0]));

    static QEvtPtr video_queue_sto[VIDEO_AO_QUEUE_LEN];
    video_ao_ctor();
    QActive_start(AO_Video,
                  VIDEO_AO_PRIO,
                  video_queue_sto,
                  Q_DIM(video_queue_sto),
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

    static QEvtPtr system_queue_sto[SYSTEM_AO_QUEUE_LEN];
    system_ao_ctor();
    QActive_start(AO_System,
                  SYSTEM_AO_PRIO,
                  system_queue_sto,
                  Q_DIM(system_queue_sto),
                  (void*)0,
                  0U,
                  (void*)0);

    /*
     * TODO: Construct and start USBAudioAO, ButtonsAO, and LEDAO here as they
     * are implemented.
     */
}

int main(void)
{
    QF_init();
    bsp_init_placeholder();
    app_init();
    return QF_run();
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
    QTIMEEVT_TICK_X(0U, (void*)0);
}

// === End of documentation ======================================================================================== //
