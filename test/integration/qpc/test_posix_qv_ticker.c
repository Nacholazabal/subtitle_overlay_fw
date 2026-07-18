// Integration test for the posix-qv port ticker lifecycle (SRC-C01).
//
// Unlike the other QP/C tests, this one starts the REAL cooperative kernel via
// QF_run(), which spawns the REAL ticker thread. It verifies that:
//   * real QTimeEvt ticks are delivered (the ticker actually runs), and
//   * QF_run()/QF_stop() shut down cleanly and repeatably (joinable ticker).
//
// The upstream 8.1.4 port set `l_isRunning = true` only AFTER creating the
// ticker; if the ticker won the race it saw `false` and exited, so NO tick was
// ever delivered. Each cycle runs in a fresh forked process so repeated
// start/stop is exercised on clean global state, and a watchdog timeout turns a
// regressed hang into a test failure instead of a stuck suite.

#include "unity.h"

#include "app.h"
#include "qpc_test_harness.h"
#include "qp_port.h"
#include "qp_pkg.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

TEST_SOURCE_FILE("qpc_test_harness.c")
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

// Private signal for the tick-counting AO (past the application signal space).
enum
{
    TICK_TEST_SIG = MAX_SIG
};

typedef struct
{
    QActive super;
    QTimeEvt tick_evt;
    uint32_t ticks;
} tick_counter_ao_t;

static QState tick_counter_initial(tick_counter_ao_t* const me, void const* const par);
static QState tick_counter_active(tick_counter_ao_t* const me, QEvt const* const e);

static QState tick_counter_initial(tick_counter_ao_t* const me, void const* const par)
{
    Q_UNUSED_PAR(par);
    // Periodic time event: fire on every tick so we detect the ticker running.
    QTimeEvt_armX(&me->tick_evt, 1U, 1U);
    return Q_TRAN(&tick_counter_active);
}

static QState tick_counter_active(tick_counter_ao_t* const me, QEvt const* const e)
{
    switch (e->sig)
    {
    case TICK_TEST_SIG:
        ++me->ticks;
        return Q_HANDLED();
    default:
        return Q_SUPER(&QHsm_top);
    }
}

static void tick_counter_ctor(tick_counter_ao_t* const me)
{
    me->ticks = 0U;
    QActive_ctor(&me->super, Q_STATE_CAST(&tick_counter_initial));
    QTimeEvt_ctorX(&me->tick_evt, &me->super, TICK_TEST_SIG, 0U);
}

static unsigned g_run_ms;

static void* stopper_thread(void* arg)
{
    Q_UNUSED_PAR(arg);
    struct timespec ts = {
        .tv_sec = (time_t)(g_run_ms / 1000U),
        .tv_nsec = (long)(g_run_ms % 1000U) * 1000000L,
    };
    (void)nanosleep(&ts, (struct timespec*)0);
    QF_stop();
    return (void*)0;
}

// Runs a single kernel lifecycle. Returns 0 on success (ticks delivered and
// clean shutdown), non-zero otherwise. Called only inside a forked child.
static int run_one_cycle(unsigned run_ms)
{
    static tick_counter_ao_t ao;
    static QEvtPtr queue[8];

    g_run_ms = run_ms;

    qpc_test_init();
    QF_setTickRate(1000U, 0); // 1 kHz -> 1 ms ticks so the test stays fast
    tick_counter_ctor(&ao);
    qpc_test_start(&ao.super, 1U, queue, Q_DIM(queue));

    pthread_t th;
    if (pthread_create(&th, (pthread_attr_t*)0, &stopper_thread, (void*)0) != 0)
    {
        return 3;
    }

    QF_run(); // blocks until the stopper thread calls QF_stop()
    (void)pthread_join(th, (void**)0);

    return (ao.ticks > 0U) ? 0 : 1; // real ticks must have been delivered
}

// Forks a child that runs one kernel cycle and asserts it exited 0 within the
// watchdog. A regressed start-up race (no ticks) fails with rc 1; a regressed
// shutdown hang is caught by the watchdog kill.
static void run_cycle_in_child(unsigned run_ms, unsigned timeout_ms)
{
    fflush(stdout); // avoid the child re-flushing the parent's buffered output
    fflush(stderr);

    pid_t pid = fork();
    TEST_ASSERT_TRUE_MESSAGE(pid >= 0, "fork failed");

    if (pid == 0)
    {
        int rc = run_one_cycle(run_ms);
        // _exit() (not exit()) so forked children do not race on the shared
        // gcov .gcda files or re-flush the parent's stdio buffers.
        _exit(rc);
    }

    unsigned waited_ms = 0U;
    int status = 0;
    for (;;)
    {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
        {
            break;
        }
        if (waited_ms >= timeout_ms)
        {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            TEST_FAIL_MESSAGE("QF_run/QF_stop cycle did not terminate (hang)");
            return;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
        (void)nanosleep(&ts, (struct timespec*)0);
        waited_ms += 5U;
    }

    TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status), "kernel cycle child crashed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(status),
                                  "kernel cycle delivered no ticks or failed to shut down");
}

void setUp(void)
{
}

void tearDown(void)
{
}

// The ticker must deliver real QTimeEvt ticks and shut down cleanly.
void test_ticker_delivers_real_ticks_and_shuts_down_cleanly(void)
{
    run_cycle_in_child(60U, 3000U);
}

// Repeated start/stop must always deliver ticks. Short run windows probe the
// former start-up race (ticker created before the running flag was published)
// many times over.
void test_repeated_start_stop_always_delivers_ticks(void)
{
    for (unsigned i = 0U; i < 8U; ++i)
    {
        run_cycle_in_child(20U, 3000U);
    }
}
