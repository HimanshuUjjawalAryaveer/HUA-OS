
#include "tests/scheduler_sleep_tests.h"

#include <stdbool.h>
#include <stdint.h>

#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "time/time.h"
#include "arch/x86_64/irq_flags.h"
#include "drivers/console/console.h"

/*
 * Three workers use absolute deadlines separated by 100 ms.
 * The LAPIC timer currently checks deadlines at 100 Hz.
 */
#define SLEEP_TEST_WORKER_COUNT       3U
#define SLEEP_TEST_FIRST_DELAY_NS     200000000ULL
#define SLEEP_TEST_DEADLINE_STEP_NS   100000000ULL
#define SLEEP_TEST_FINAL_WAIT_NS      500000000ULL
#define SLEEP_TEST_IDLE_DURATION_NS   30000000ULL

struct scheduler_sleep_worker_argument {
    uint64_t deadline_ns;
    uint32_t slot;
};

struct scheduler_sleep_test_state {
    volatile uint32_t completed;
    volatile uint32_t failed;

    volatile uint32_t completion_order[SLEEP_TEST_WORKER_COUNT];

    uint64_t wake_time_ns[SLEEP_TEST_WORKER_COUNT];
};

static struct scheduler_sleep_test_state sleep_test_state;

static struct scheduler_sleep_worker_argument sleep_test_arguments[
    SLEEP_TEST_WORKER_COUNT
];


/*
 * Each worker sleeps until its assigned absolute deadline.
 *
 * On resumption, it records the actual wake time and
 * the order in which it completed.
 */
static void scheduler_sleep_test_worker(void *argument) {

    struct scheduler_sleep_worker_argument *worker =
        (struct scheduler_sleep_worker_argument *)argument;

    if (worker == NULL ||
        worker->slot >= SLEEP_TEST_WORKER_COUNT) {

        __atomic_store_n(
            &sleep_test_state.failed,
            1,
            __ATOMIC_RELEASE
        );

        return;
    }

    uint32_t slot = worker->slot;

    console_write("[ WORKER ] Starting sleep\n");

    if (!thread_sleep_until(worker->deadline_ns)) {

        __atomic_store_n(
            &sleep_test_state.failed,
            1,
            __ATOMIC_RELEASE
        );

        return;
    }

    console_write("[ WORKER ] Sleep returned successfully\n");

    uint64_t wake_time_ns = 0;

    if (time_get_monotonic_ns(&wake_time_ns) != TIME_STATUS_OK ||
        wake_time_ns < worker->deadline_ns) {

        __atomic_store_n(
            &sleep_test_state.failed,
            1,
            __ATOMIC_RELEASE
        );
    }

    sleep_test_state.wake_time_ns[slot] = wake_time_ns;

    uint32_t order = __atomic_add_fetch(
        &sleep_test_state.completed,
        1,
        __ATOMIC_ACQ_REL
    );

    __atomic_store_n(
        &sleep_test_state.completion_order[slot],
        order,
        __ATOMIC_RELEASE
    );

    console_write("[ WORKER ] Completed\n");
}


/*
 * Verify the immediate-return cases.
 */
static bool scheduler_sleep_test_immediate_cases(void) {

    uint64_t now_ns = 0;

    if (time_get_monotonic_ns(&now_ns) != TIME_STATUS_OK ||
        now_ns == 0) {

        return false;
    }

    /*
     * Zero-duration sleep must not block.
     */
    if (!thread_sleep(0)) {
        return false;
    }

    /*
     * An already-expired absolute deadline must not block.
     */
    if (!thread_sleep_until(now_ns)) {
        return false;
    }

    /*
     * Adding UINT64_MAX to the current time must be
     * rejected rather than overflowing the deadline.
     */
    if (thread_sleep(UINT64_MAX)) {
        return false;
    }

    return true;
}


/*
 * Verify that the bootstrap thread can sleep while no
 * other ordinary thread is runnable.
 *
 * The idle thread must execute until the LAPIC timer
 * wakes the sleeping bootstrap thread.
 */
static bool scheduler_sleep_test_idle_wakeup(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap
) {

    if (cpu == NULL ||
        bootstrap == NULL ||
        cpu->current_thread != bootstrap ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->sleep_queue.count != 0) {

        return false;
    }

    uint64_t initial_ticks = cpu->timer_ticks;
    uint64_t initial_switches = cpu->context_switch_count;

    if (initial_switches > UINT64_MAX - 2) {
        return false;
    }

    /*
     * This should switch:
     *
     * bootstrap -> idle
     * idle -> bootstrap (after timer expiration)
     */
    if (!thread_sleep(SLEEP_TEST_IDLE_DURATION_NS)) {
        return false;
    }

    if (cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        return false;
    }

    if (cpu->timer_ticks <= initial_ticks) {
        return false;
    }

    if (cpu->context_switch_count < initial_switches + 2) {
        return false;
    }

    if (cpu->sleep_queue.count != 0 ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0) {

        return false;
    }

    return true;
}


/*
 * Verify multiple real threads sleeping until different
 * deadlines and waking in deadline order.
 */
static bool scheduler_sleep_test_multiple_workers(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap
) {

    if (cpu == NULL ||
        bootstrap == NULL ||
        cpu->current_thread != bootstrap ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->sleep_queue.count != 0 ||
        cpu->reap_count != 0) {

        return false;
    }

    uint64_t base_ns = 0;

    if (time_get_monotonic_ns(&base_ns) != TIME_STATUS_OK) {
        return false;
    }

    if (base_ns > UINT64_MAX - SLEEP_TEST_FINAL_WAIT_NS) {
        return false;
    }

    /*
     * Reset all shared test state.
     */
    __atomic_store_n(
        &sleep_test_state.completed,
        0,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &sleep_test_state.failed,
        0,
        __ATOMIC_RELEASE
    );

    for (uint32_t i = 0; i < SLEEP_TEST_WORKER_COUNT; i++) {

        __atomic_store_n(
            &sleep_test_state.completion_order[i],
            0,
            __ATOMIC_RELEASE
        );

        sleep_test_state.wake_time_ns[i] = 0;

        uint64_t delay_ns =
            SLEEP_TEST_FIRST_DELAY_NS +
            ((uint64_t)i * SLEEP_TEST_DEADLINE_STEP_NS);

        if (delay_ns > UINT64_MAX - base_ns) {
            return false;
        }

        sleep_test_arguments[i].deadline_ns =
            base_ns + delay_ns;

        sleep_test_arguments[i].slot = i;
    }

    /*
     * Create all workers before enqueuing them.
     */
    struct thread *workers[SLEEP_TEST_WORKER_COUNT] = {
        NULL,
        NULL,
        NULL
    };

    for (uint32_t i = 0; i < SLEEP_TEST_WORKER_COUNT; i++) {

        console_write("[ SLEEP TEST ] Creating worker\n");
        workers[i] = thread_create(
            "sleep-test",
            scheduler_sleep_test_worker,
            &sleep_test_arguments[i],
            0
        );

        if (workers[i] == NULL) {

            for (uint32_t j = 0; j < i; j++) {
                (void)thread_destroy(workers[j]);
            }

            return false;
        }

        console_write("[ SLEEP TEST ] Worker created\n");
    }

    console_write("[ SLEEP TEST ] Enqueueing workers...\n");

    /*
     * Make all three workers runnable.
     */
    for (uint32_t i = 0; i < SLEEP_TEST_WORKER_COUNT; i++) {

        if (!scheduler_enqueue_thread(0, workers[i])) {

            /*
             * A partially enqueued test cannot safely
             * continue. The caller will stop the tests.
             */
            return false;
        }
    }

    console_write("[ SLEEP TEST ] All workers enqueued\n");

    /*
     * Sleep the bootstrap thread beyond the latest worker
     * deadline. This lets the workers sleep, wake, finish,
     * and terminate while the idle thread handles the gaps.
     */
    uint64_t final_deadline =
        base_ns + SLEEP_TEST_FINAL_WAIT_NS;

    console_write("[ SLEEP TEST ] Bootstrap sleeping...\n");

    if (!thread_sleep_until(final_deadline)) {
        return false;
    }

    console_write("[ SLEEP TEST ] Bootstrap resumed\n");

    /*
     * The bootstrap thread must have resumed.
     */
    if (cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        return false;
    }

    /*
     * All workers must have completed.
     */
    if (__atomic_load_n(
            &sleep_test_state.completed,
            __ATOMIC_ACQUIRE
        ) != SLEEP_TEST_WORKER_COUNT) {

        return false;
    }

    if (__atomic_load_n(
            &sleep_test_state.failed,
            __ATOMIC_ACQUIRE
        ) != 0) {

        return false;
    }

    /*
     * Verify actual completion order and no early wakeups.
     */
    for (uint32_t i = 0; i < SLEEP_TEST_WORKER_COUNT; i++) {

        if (__atomic_load_n(
                &sleep_test_state.completion_order[i],
                __ATOMIC_ACQUIRE
            ) != i + 1) {

            return false;
        }

        if (sleep_test_state.wake_time_ns[i] <
            sleep_test_arguments[i].deadline_ns) {

            return false;
        }
    }

    /*
     * thread_sleep_until() reaps terminated threads
     * after the bootstrap thread resumes.
     */
    if (cpu->sleep_queue.count != 0 ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    return true;
}


/*
 * Run the real timed-sleep integration tests.
 *
 * Must be called after the LAPIC periodic timer starts
 * and CPU interrupts are enabled.
 */
bool scheduler_sleep_run_tests(void) {

    if (!scheduler_is_initialized()) {
        return false;
    }

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        cpu->current_thread == NULL) {

        return false;
    }

    struct thread *bootstrap =
        cpu->current_thread;

    if (bootstrap->state != THREAD_STATE_RUNNING ||
        bootstrap->scheduling_class == THREAD_CLASS_IDLE ||
        bootstrap->preempt_count != 0) {

        return false;
    }

    /*
     * The test suite expects a clean scheduler.
     */
    if (cpu->run_queue.runnable_count != 0 ||
        cpu->sleep_queue.count != 0 ||
        cpu->reap_count != 0) {

        return false;
    }

    console_write("[ SLEEP TEST ] Immediate cases...\n");

    if (!scheduler_sleep_test_immediate_cases()) {
        console_write("[ SLEEP TEST ] Immediate cases FAILED\n");
        return false;
    }

    console_write("[ SLEEP TEST ] Immediate cases PASSED\n");
    console_write("[ SLEEP TEST ] Idle wakeup starting...\n");

    if (!scheduler_sleep_test_idle_wakeup(
            cpu,
            bootstrap
        )) {

        console_write("[ SLEEP TEST ] Idle wakeup FAILED\n");
        return false;
    }

    console_write("[ SLEEP TEST ] Idle wakeup PASSED\n");
    console_write("[ SLEEP TEST ] Multiple workers starting...\n");

    if (!scheduler_sleep_test_multiple_workers(
            cpu,
            bootstrap
        )) {

        console_write("[ SLEEP TEST ] Multiple workers FAILED\n");
        return false;
    }

    console_write("[ SLEEP TEST ] Multiple workers PASSED\n");

    /*
     * Final scheduler-state verification.
     */
    if (cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->sleep_queue.count != 0 ||
        cpu->reap_count != 0) {

        return false;
    }

    return true;
}
