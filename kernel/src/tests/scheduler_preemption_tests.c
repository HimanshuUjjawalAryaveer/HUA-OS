
#include "tests/scheduler_preemption_tests.h"

#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "arch/x86_64/irq_flags.h"
#include "drivers/console/console.h"

/*
 * Bound test execution so a broken timer or scheduler
 * cannot leave a CPU-bound test spinning forever.
 */
#define PREEMPT_TEST_TIMEOUT_TICKS  30ULL
#define PREEMPT_TEST_LOOP_LIMIT     10000000ULL

struct preemption_pair {

    volatile uint32_t started[2];

    volatile uint32_t finished;

    volatile bool failed;
};

struct preemption_pair_worker_argument {

    struct preemption_pair *pair;

    uint32_t slot;
};

struct preemption_statistics {

    uint64_t context_switches;

    uint64_t voluntary_yields;

};

/*
 * Read timer ticks without allowing the timer interrupt
 * to interrupt the read.
 */
static uint64_t preemption_test_read_ticks(void) {

    uint64_t flags = arch_irq_save_disable();

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    uint64_t ticks = cpu != NULL ? cpu->timer_ticks : 0;

    arch_irq_restore(flags);

    return ticks;
}


/*
 * Take a consistent snapshot of scheduler counters.
 */
static bool preemption_test_read_statistics(
    struct preemption_statistics *statistics
) {

    if (statistics == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL) {

        arch_irq_restore(flags);

        return false;
    }

    statistics->context_switches =
        cpu->context_switch_count;

    statistics->voluntary_yields =
        cpu->voluntary_yield_count;

    arch_irq_restore(flags);

    return true;
}


/*
 * Wait for a specified number of timer ticks.
 *
 * The loop limit is a secondary timeout in case timer
 * interrupts stop arriving.
 */
static bool preemption_test_wait_ticks(uint64_t count) {

    uint64_t start = preemption_test_read_ticks();

    for (uint64_t loops = 0;
         loops < PREEMPT_TEST_LOOP_LIMIT;
         loops++) {

        uint64_t current = preemption_test_read_ticks();

        if (current - start >= count) {
            return true;
        }

        asm volatile ("pause");
    }

    return false;
}


/*
 * Wait for two CPU-bound threads to observe each other.
 *
 * Neither worker calls thread_yield().
 *
 * Therefore, both workers can complete this handshake
 * only if the scheduler actually gives CPU time to both.
 */
static void preemption_test_pair_worker(void *argument) {

    struct preemption_pair_worker_argument *worker =
        (struct preemption_pair_worker_argument *)argument;

    if (worker == NULL ||
        worker->pair == NULL ||
        worker->slot > 1) {

        return;
    }

    struct preemption_pair *pair = worker->pair;

    uint32_t own_slot = worker->slot;
    uint32_t other_slot = own_slot ^ 1U;

    __atomic_store_n(
        &pair->started[own_slot],
        1,
        __ATOMIC_RELEASE
    );

    uint64_t start_ticks =
        preemption_test_read_ticks();

    bool observed_peer = false;

    for (uint64_t loops = 0;
         loops < PREEMPT_TEST_LOOP_LIMIT;
         loops++) {

        if (__atomic_load_n(
                &pair->started[other_slot],
                __ATOMIC_ACQUIRE
            ) != 0) {

            observed_peer = true;
            break;
        }

        if (preemption_test_read_ticks() - start_ticks >=
            PREEMPT_TEST_TIMEOUT_TICKS) {

            break;
        }

        asm volatile ("pause");
    }

    if (!observed_peer) {

        __atomic_store_n(
            &pair->failed,
            true,
            __ATOMIC_RELEASE
        );
    }

    __atomic_fetch_add(
        &pair->finished,
        1,
        __ATOMIC_RELEASE
    );

    /*
     * Returning from this function causes thread_bootstrap()
     * to call thread_exit().
     */
}


/*
 * Run two CPU-bound threads and verify that they both
 * execute without voluntary yielding.
 */
static bool preemption_test_run_pair(
    const char *first_name,
    const char *second_name,
    bool realtime,
    uint8_t priority,
    thread_rt_policy_t policy,
    uint64_t quantum_ns
) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        cpu->current_thread == NULL ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0) {

        return false;
    }

    struct preemption_pair pair = {0};

    struct preemption_pair_worker_argument arguments[2] = {
        { &pair, 0 },
        { &pair, 1 }
    };

    struct preemption_statistics before;
    struct preemption_statistics after;

    if (!preemption_test_read_statistics(&before)) {
        return false;
    }

    struct thread *first = thread_create(
        first_name,
        preemption_test_pair_worker,
        &arguments[0],
        0
    );

    if (first == NULL) {
        return false;
    }

    struct thread *second = thread_create(
        second_name,
        preemption_test_pair_worker,
        &arguments[1],
        0
    );

    if (second == NULL) {

        thread_destroy(first);

        return false;
    }

    if (realtime) {

        if (!thread_set_realtime(
                first,
                priority,
                policy,
                quantum_ns
            ) ||
            !thread_set_realtime(
                second,
                priority,
                policy,
                quantum_ns
            )) {

            thread_destroy(first);
            thread_destroy(second);

            return false;
        }
    }

    if (!scheduler_enqueue_thread(0, first) ||
        !scheduler_enqueue_thread(0, second)) {

        /*
         * A partially enqueued test is fatal to this
         * test sequence. The caller will stop the tests.
         */
        return false;
    }

    /*
     * Start the first worker.
     *
     * The other worker remains runnable. The running
     * worker cannot finish its handshake without the
     * scheduler switching to its peer.
     */
    if (!thread_yield()) {
        return false;
    }

    /*
     * The bootstrap thread may resume before every worker
     * has finished. Continue waiting while timer preemption
     * allows the remaining worker to execute.
     */
    uint64_t start_ticks =
        preemption_test_read_ticks();

    for (uint64_t loops = 0;
         loops < PREEMPT_TEST_LOOP_LIMIT;
         loops++) {

        if (__atomic_load_n(
                &pair.finished,
                __ATOMIC_ACQUIRE
            ) == 2) {

            break;
        }

        if (preemption_test_read_ticks() - start_ticks >=
            PREEMPT_TEST_TIMEOUT_TICKS) {

            break;
        }

        asm volatile ("pause");
    }

    bool both_finished =
        __atomic_load_n(
            &pair.finished,
            __ATOMIC_ACQUIRE
        ) == 2;

    bool worker_failed =
        __atomic_load_n(
            &pair.failed,
            __ATOMIC_ACQUIRE
        );

    /*
     * Reclaim terminated workers from ordinary thread
     * context, never from the timer interrupt.
     */
    scheduler_reap_terminated_threads();

    if (!preemption_test_read_statistics(&after)) {
        return false;
    }

    uint64_t context_switch_delta =
        after.context_switches - before.context_switches;

    uint64_t voluntary_yield_delta =
        after.voluntary_yields - before.voluntary_yields;

    return both_finished &&
           !worker_failed &&
           context_switch_delta > voluntary_yield_delta &&
           cpu->current_thread != NULL &&
           cpu->current_thread->state == THREAD_STATE_RUNNING &&
           cpu->run_queue.runnable_count == 0 &&
           cpu->reap_count == 0 &&
           cpu->reap_head == NULL &&
           cpu->reap_tail == NULL;
}


/*
 * Verify fair-class CPU-bound preemption.
 */
static bool preemption_test_fair(void) {

    return preemption_test_run_pair(
        "fair-preempt-a",
        "fair-preempt-b",
        false,
        0,
        THREAD_RT_FIFO,
        0
    );
}


/*
 * A high-priority RT worker must preempt the running
 * fair bootstrap thread.
 */
static void preemption_test_rt_worker(void *argument) {

    volatile bool *executed = (volatile bool *)argument;

    if (executed != NULL) {
        __atomic_store_n(executed, true, __ATOMIC_RELEASE);
    }
}


/*
 * Verify real-time priority preemption.
 */
static bool preemption_test_rt_priority(void) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        cpu->current_thread == NULL ||
        cpu->run_queue.runnable_count != 0) {

        return false;
    }

    volatile bool executed = false;

    struct preemption_statistics before;
    struct preemption_statistics after;

    if (!preemption_test_read_statistics(&before)) {
        return false;
    }

    struct thread *worker = thread_create(
        "rt-priority-test",
        preemption_test_rt_worker,
        (void *)&executed,
        0
    );

    if (worker == NULL) {
        return false;
    }

    if (!thread_set_realtime(
            worker,
            THREAD_RT_PRIORITY_LEVELS - 1U,
            THREAD_RT_FIFO,
            0
        )) {

        thread_destroy(worker);
        return false;
    }

    if (!scheduler_enqueue_thread(0, worker)) {
        return false;
    }

    /*
     * Deliberately do not yield.
     *
     * The high-priority RT worker must run because
     * the LAPIC timer preempts this fair thread.
     */
    uint64_t start_ticks =
        preemption_test_read_ticks();

    for (uint64_t loops = 0;
         loops < PREEMPT_TEST_LOOP_LIMIT;
         loops++) {

        if (__atomic_load_n(
                &executed,
                __ATOMIC_ACQUIRE
            )) {

            break;
        }

        if (preemption_test_read_ticks() - start_ticks >=
            PREEMPT_TEST_TIMEOUT_TICKS) {

            break;
        }

        asm volatile ("pause");
    }

    bool success =
        __atomic_load_n(&executed, __ATOMIC_ACQUIRE);

    scheduler_reap_terminated_threads();

    if (!preemption_test_read_statistics(&after)) {
        return false;
    }

    uint64_t switch_delta =
        after.context_switches - before.context_switches;

    return success &&
           switch_delta >= 2 &&
           cpu->current_thread != NULL &&
           cpu->current_thread->state == THREAD_STATE_RUNNING &&
           cpu->run_queue.runnable_count == 0 &&
           cpu->reap_count == 0;
}


/*
 * Verify that equal-priority RR threads rotate after
 * consuming their configured quantum.
 */
static bool preemption_test_round_robin(void) {

    return preemption_test_run_pair(
        "rt-rr-a",
        "rt-rr-b",
        true,
        12,
        THREAD_RT_ROUND_ROBIN,
        THREAD_RT_DEFAULT_QUANTUM_NS
    );
}


/*
 * Verify that preemption protection suppresses a required
 * real-time preemption until the protection is released.
 */
static bool preemption_test_preempt_disable(void) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        cpu->current_thread == NULL ||
        cpu->run_queue.runnable_count != 0) {

        return false;
    }

    volatile bool executed = false;

    struct thread *worker = thread_create(
        "preempt-guard-test",
        preemption_test_rt_worker,
        (void *)&executed,
        0
    );

    if (worker == NULL) {
        return false;
    }

    if (!thread_set_realtime(
            worker,
            THREAD_RT_PRIORITY_LEVELS - 1U,
            THREAD_RT_FIFO,
            0
        )) {

        thread_destroy(worker);
        return false;
    }

    if (!scheduler_enqueue_thread(0, worker)) {
        return false;
    }

    struct thread *bootstrap = cpu->current_thread;

    if (!thread_preempt_disable()) {
        return false;
    }

    uint64_t start_ticks =
        preemption_test_read_ticks();

    bool waited =
        preemption_test_wait_ticks(3);

    bool remained_current =
        cpu->current_thread == bootstrap &&
        bootstrap->preempt_count == 1;

    bool worker_stayed_blocked =
        !__atomic_load_n(&executed, __ATOMIC_ACQUIRE);

    bool enabled = thread_preempt_enable();

    /*
     * After protection is released, the pending scheduling
     * decision must be handled at a later timer boundary.
     */
    uint64_t release_ticks =
        preemption_test_read_ticks();

    bool executed_after_release = false;

    for (uint64_t loops = 0;
         loops < PREEMPT_TEST_LOOP_LIMIT;
         loops++) {

        if (__atomic_load_n(
                &executed,
                __ATOMIC_ACQUIRE
            )) {

            executed_after_release = true;
            break;
        }

        if (preemption_test_read_ticks() - release_ticks >=
            PREEMPT_TEST_TIMEOUT_TICKS) {

            break;
        }

        asm volatile ("pause");
    }

    scheduler_reap_terminated_threads();

    return waited &&
           remained_current &&
           worker_stayed_blocked &&
           enabled &&
           executed_after_release &&
           bootstrap->preempt_count == 0 &&
           cpu->current_thread == bootstrap &&
           cpu->run_queue.runnable_count == 0 &&
           cpu->reap_count == 0;
}


/*
 * Run real timer-driven scheduler tests.
 *
 * Caller must have started the clockevent and enabled
 * CPU interrupt delivery.
 */
bool scheduler_preemption_run_tests(void) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        cpu->current_thread == NULL ||
        cpu->current_thread->state != THREAD_STATE_RUNNING ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0) {

        console_write("[ FAILED ] Preemption test prerequisites\n");
        return false;
    }

#define RUN_PREEMPTION_TEST(name, function) \
    do { \
        console_write("[ TEST ] " name "\n"); \
        if (!(function)()) { \
            console_write("[ FAILED ] " name "\n"); \
            return false; \
        } \
        console_write("[ OK ] " name "\n"); \
    } while (0)

    RUN_PREEMPTION_TEST(
        "CPU-bound fair preemption",
        preemption_test_fair
    );

    RUN_PREEMPTION_TEST(
        "Real-time priority preemption",
        preemption_test_rt_priority
    );

    RUN_PREEMPTION_TEST(
        "Real-time round-robin rotation",
        preemption_test_round_robin
    );

    RUN_PREEMPTION_TEST(
        "Preemption-disable protection",
        preemption_test_preempt_disable
    );

#undef RUN_PREEMPTION_TEST

    return true;
}
