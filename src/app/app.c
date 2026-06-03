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
#include "SystemAO.h"

// === Macros definitions ========================================================================================== //

#define APP_TICKS_PER_SEC   (100U)
#define APP_ERROR_POOL_LEN  (8U)
#define SYSTEM_AO_QUEUE_LEN (8U)
#define SYSTEM_AO_PRIO      (1U)

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
    /*
     * Error reports are the first dynamic events in the application. Components
     * can allocate one with Q_NEW(app_error_evt_t, COMPONENT_ERROR_SIG), fill its
     * source and code fields, then post &event->super to AO_System.
     */
    static QF_MPOOL_EL(app_error_evt_t) app_error_pool_sto[APP_ERROR_POOL_LEN];
    QF_poolInit(app_error_pool_sto, sizeof(app_error_pool_sto), sizeof(app_error_pool_sto[0]));

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
     * TODO: Construct and start VideoAO, USBAudioAO, SubtitlePipelineAO,
     * ButtonsAO, and LEDAO here as they are implemented.
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
