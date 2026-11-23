
#include "tests/scheduler_tests.h"

#include "scheduler/scheduler.h"
#include "scheduler/fair.h"
#include "drivers/console/console.h"

/*
 * Prepare a synthetic thread for run-queue testing.
 *
 * These test threads do not own stacks and are never executed.
 */
static void scheduler_test_thread_init(
    struct thread *thread,
    thread_id_t id,
    thread_class_t scheduling_class,
    uint8_t priority,
    uint64_t virtual_deadline,
    uint64_t virtual_runtime
) {

    *thread = (struct thread){0};

    thread->id = id;
    thread->state = THREAD_STATE_NEW;

    thread->scheduling_class = scheduling_class;

    thread->realtime_priority = priority;

    thread->fair_weight = THREAD_FAIR_WEIGHT_DEFAULT;

    thread->fair_slice_ns =
        THREAD_FAIR_DEFAULT_SLICE_NS;

    thread->virtual_deadline = virtual_deadline;
    thread->virtual_runtime = virtual_runtime;

    thread->affinity_mask = 1ULL;
}


/*
 * Verify queue initialization.
 */
static bool scheduler_test_queue_init(void) {

    struct scheduler_run_queue queue;

    scheduler_run_queue_init(&queue);

    if (queue.runnable_count != 0 ||
        queue.fair.head != NULL ||
        queue.fair.tail != NULL ||
        queue.fair.count != 0) {

        return false;
    }

    for (uint32_t i = 0;
         i < THREAD_RT_PRIORITY_LEVELS;
         i++) {

        if (queue.realtime[i].head != NULL ||
            queue.realtime[i].tail != NULL ||
            queue.realtime[i].count != 0) {

            return false;
        }
    }

    return true;
}


/*
 * Verify FIFO ordering inside one real-time priority.
 */
static bool scheduler_test_realtime_fifo(void) {

    struct scheduler_run_queue queue;

    struct thread first;
    struct thread second;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &first,
        1,
        THREAD_CLASS_REALTIME,
        12,
        0,
        0
    );

    scheduler_test_thread_init(
        &second,
        2,
        THREAD_CLASS_REALTIME,
        12,
        0,
        0
    );

    if (!scheduler_run_queue_enqueue(&queue, &first) ||
        !scheduler_run_queue_enqueue(&queue, &second)) {

        return false;
    }

    if (queue.runnable_count != 2 ||
        first.state != THREAD_STATE_READY ||
        second.state != THREAD_STATE_READY) {

        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &first) {
        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &second) {
        return false;
    }

    return queue.runnable_count == 0 &&
           first.run_queue_owner == NULL &&
           second.run_queue_owner == NULL;
}


/*
 * Verify higher real-time priority wins.
 */
static bool scheduler_test_realtime_priority(void) {

    struct scheduler_run_queue queue;

    struct thread low;
    struct thread high;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &low,
        3,
        THREAD_CLASS_REALTIME,
        5,
        0,
        0
    );

    scheduler_test_thread_init(
        &high,
        4,
        THREAD_CLASS_REALTIME,
        25,
        0,
        0
    );

    if (!scheduler_run_queue_enqueue(&queue, &low) ||
        !scheduler_run_queue_enqueue(&queue, &high)) {

        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &high) {
        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &low) {
        return false;
    }

    return queue.runnable_count == 0;
}


static bool scheduler_test_fair_deadline(void) {

    struct scheduler_run_queue queue;

    struct thread later;
    struct thread earlier;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &later,
        5,
        THREAD_CLASS_FAIR,
        0,
        0,
        1000
    );

    scheduler_test_thread_init(
        &earlier,
        6,
        THREAD_CLASS_FAIR,
        0,
        0,
        1000
    );

    later.fair_slice_ns = 9000000ULL;
    earlier.fair_slice_ns = 3000000ULL;

    if (!scheduler_run_queue_enqueue(&queue, &later)) {
        console_write("[ DEBUG ] Later thread enqueue failed\n");
        return false;
    }

    if (!scheduler_run_queue_enqueue(&queue, &earlier)) {
        console_write("[ DEBUG ] Earlier thread enqueue failed\n");
        return false;
    }

    if (later.virtual_deadline <= earlier.virtual_deadline) {
        console_write("[ DEBUG ] Virtual deadline calculation failed\n");
        return false;
    }

    if (scheduler_fair_pick(&queue) != &earlier) {
        console_write("[ DEBUG ] Direct fair selection failed\n");
        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &earlier) {
        console_write("[ DEBUG ] First run-queue selection failed\n");
        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &later) {
        console_write("[ DEBUG ] Second run-queue selection failed\n");
        return false;
    }

    if (queue.runnable_count != 0) {
        console_write("[ DEBUG ] Run-queue count incorrect\n");
        return false;
    }

    return true;
}


/*
 * Verify fair scheduling-request expiration and
 * fair-to-fair preemption decisions.
 *
 * These tests exercise scheduling policy without
 * performing a real context switch.
 */
static bool scheduler_test_fair_slice_preemption(void) {

    struct scheduler_run_queue queue;

    struct thread current;
    struct thread peer;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &current,
        30,
        THREAD_CLASS_FAIR,
        0,
        0,
        0
    );

    scheduler_test_thread_init(
        &peer,
        31,
        THREAD_CLASS_FAIR,
        0,
        0,
        0
    );

    /*
     * The current thread is running and has a 6 ms
     * fair scheduling request.
     */
    current.state = THREAD_STATE_RUNNING;

    current.fair_slice_ns =
        THREAD_FAIR_DEFAULT_SLICE_NS;

    current.slice_runtime_ns =
        THREAD_FAIR_DEFAULT_SLICE_NS - 1;

    /*
     * A slice that has not expired must not trigger
     * fair preemption.
     */
    if (scheduler_fair_slice_expired(&current)) {
        return false;
    }

    if (scheduler_fair_should_preempt(&current, &queue)) {
        return false;
    }

    /*
     * Add another fair thread to the runnable queue.
     */
    if (!scheduler_run_queue_enqueue(&queue, &peer)) {
        return false;
    }

    /*
     * The peer is runnable, but the current thread's
     * scheduling request has not expired yet.
     */
    if (scheduler_fair_should_preempt(&current, &queue)) {
        return false;
    }

    /*
     * Reaching the exact request length means that
     * the scheduling request has expired.
     */
    current.slice_runtime_ns =
        current.fair_slice_ns;

    if (!scheduler_fair_slice_expired(&current)) {
        return false;
    }

    if (!scheduler_fair_should_preempt(&current, &queue)) {
        return false;
    }

    /*
     * An expired fair thread without another fair
     * runnable thread must not trigger fair rotation.
     */
    if (!scheduler_run_queue_dequeue(&queue, &peer)) {
        return false;
    }

    if (scheduler_fair_should_preempt(&current, &queue)) {
        return false;
    }

    /*
     * Real-time threads must never be treated as fair
     * scheduling candidates by this policy helper.
     */
    current.scheduling_class = THREAD_CLASS_REALTIME;

    if (scheduler_fair_slice_expired(&current)) {
        return false;
    }

    if (scheduler_fair_should_preempt(&current, &queue)) {
        return false;
    }

    /*
     * A zero-length fair request is invalid for expiration.
     */
    current.scheduling_class = THREAD_CLASS_FAIR;
    current.fair_slice_ns = 0;

    if (scheduler_fair_slice_expired(&current)) {
        return false;
    }

    /*
     * Restore the original fair request configuration.
     */
    current.fair_slice_ns =
        THREAD_FAIR_DEFAULT_SLICE_NS;

    current.slice_runtime_ns =
        current.fair_slice_ns + 1;

    if (!scheduler_fair_slice_expired(&current)) {
        return false;
    }

    /*
     * The helper must not change queue membership.
     */
    if (queue.runnable_count != 0 ||
        peer.run_queue_owner != NULL) {

        return false;
    }

    return true;
}


/*
 * Verify that a fair thread preserves its existing
 * virtual deadline when a RUNNING thread is re-enqueued.
 */
static bool scheduler_test_fair_deadline_preservation(void) {

    struct scheduler_run_queue queue;
    struct thread thread;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &thread,
        32,
        THREAD_CLASS_FAIR,
        0,
        0,
        0
    );

    thread.virtual_runtime = 1000000ULL;

    /*
     * Initial enqueue calculates the first deadline.
     */
    if (!scheduler_run_queue_enqueue(&queue, &thread)) {
        return false;
    }

    uint64_t original_deadline =
        thread.virtual_deadline;

    if (original_deadline <= thread.virtual_runtime) {
        return false;
    }

    /*
     * Remove the thread as if it were selected.
     */
    if (!scheduler_run_queue_dequeue(&queue, &thread)) {
        return false;
    }

    /*
     * Simulate a running thread being interrupted
     * before its scheduling request expires.
     */
    thread.state = THREAD_STATE_RUNNING;

    thread.virtual_runtime += 500000ULL;

    /*
     * Re-enqueue must preserve its existing deadline.
     */
    if (!scheduler_run_queue_enqueue(&queue, &thread)) {
        return false;
    }

    if (thread.virtual_deadline != original_deadline) {
        return false;
    }

    if (thread.run_queue_owner != &queue ||
        thread.state != THREAD_STATE_READY ||
        queue.runnable_count != 1) {

        return false;
    }

    /*
     * Clean up the synthetic test thread.
     */
    if (!scheduler_run_queue_dequeue(&queue, &thread)) {
        return false;
    }

    return queue.runnable_count == 0 &&
           thread.run_queue_owner == NULL;
}


/*
 * Verify weighted virtual-runtime accounting.
 *
 * At weight 1024, one millisecond of execution produces
 * one millisecond of virtual runtime.
 *
 * At weight 2048, the same execution produces half as
 * much virtual runtime.
 */
static bool scheduler_test_weighted_runtime(void) {

    struct thread normal;
    struct thread heavy;

    scheduler_test_thread_init(
        &normal,
        20,
        THREAD_CLASS_FAIR,
        0,
        0,
        0
    );

    scheduler_test_thread_init(
        &heavy,
        21,
        THREAD_CLASS_FAIR,
        0,
        0,
        0
    );

    normal.fair_weight = 1024;
    heavy.fair_weight = 2048;

    if (!scheduler_fair_account_runtime(
            &normal,
            1000000ULL
        )) {

        return false;
    }

    if (!scheduler_fair_account_runtime(
            &heavy,
            1000000ULL
        )) {

        return false;
    }

    if (normal.runtime_ns != 1000000ULL ||
        heavy.runtime_ns != 1000000ULL) {

        return false;
    }

    if (normal.virtual_runtime != 1000000ULL ||
        heavy.virtual_runtime != 500000ULL) {

        return false;
    }

    return true;
}


/*
 * Verify weighted average virtual runtime.
 */
static bool scheduler_test_weighted_average(void) {

    struct scheduler_run_queue queue;

    struct thread first;
    struct thread second;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &first,
        22,
        THREAD_CLASS_FAIR,
        0,
        0,
        1000
    );

    scheduler_test_thread_init(
        &second,
        23,
        THREAD_CLASS_FAIR,
        0,
        0,
        2000
    );

    first.fair_weight = 1024;
    second.fair_weight = 2048;

    if (!scheduler_run_queue_enqueue(&queue, &first) ||
        !scheduler_run_queue_enqueue(&queue, &second)) {

        return false;
    }

    /*
     * Weighted average:
     *
     * (1000 * 1024 + 2000 * 2048) / 3072
     *
     * The implementation calculates this relative to
     * the minimum runtime, producing 1666 by integer division.
     */
    if (queue.fair_min_vruntime != 1000 ||
        queue.fair_avg_vruntime != 1666 ||
        queue.fair_total_weight != 3072) {

        return false;
    }

    return true;
}


/*
 * Verify EEVDF eligibility.
 *
 * The third thread has the earliest virtual deadline,
 * but its virtual runtime is ahead of the average.
 * It must therefore not be selected.
 */
static bool scheduler_test_fair_eligibility(void) {

    struct scheduler_run_queue queue;

    struct thread behind;
    struct thread eligible;
    struct thread ahead;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &behind,
        24,
        THREAD_CLASS_FAIR,
        0,
        0,
        1000
    );

    scheduler_test_thread_init(
        &eligible,
        25,
        THREAD_CLASS_FAIR,
        0,
        0,
        2000
    );

    scheduler_test_thread_init(
        &ahead,
        26,
        THREAD_CLASS_FAIR,
        0,
        0,
        3000
    );

    behind.fair_slice_ns = 8000000ULL;
    eligible.fair_slice_ns = 5000000ULL;
    ahead.fair_slice_ns = 1000000ULL;

    if (!scheduler_run_queue_enqueue(&queue, &behind) ||
        !scheduler_run_queue_enqueue(&queue, &eligible) ||
        !scheduler_run_queue_enqueue(&queue, &ahead)) {

        return false;
    }

    if (queue.fair_avg_vruntime != 2000) {
        return false;
    }

    if (ahead.virtual_deadline >=
        eligible.virtual_deadline) {

        return false;
    }

    /*
     * The eligible thread must win even though another
     * thread has an earlier deadline.
     */
    if (scheduler_fair_pick(&queue) != &eligible) {
        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &eligible) {
        return false;
    }

    /*
     * Once the eligible thread is removed, the remaining
     * two-thread average is 2000.
     * The thread at 1000 is now the eligible choice.
     */
    if (scheduler_run_queue_pick_next(&queue) != &behind) {
        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &ahead) {
        return false;
    }

    return queue.runnable_count == 0;
}


/*
 * Verify weight validation.
 */
static bool scheduler_test_fair_weight_validation(void) {

    struct thread thread;

    scheduler_test_thread_init(
        &thread,
        27,
        THREAD_CLASS_FAIR,
        0,
        0,
        0
    );

    if (!scheduler_fair_set_weight(&thread, 2048)) {
        return false;
    }

    if (thread.fair_weight != 2048) {
        return false;
    }

    if (scheduler_fair_set_weight(
            &thread,
            THREAD_FAIR_WEIGHT_MAX + 1
        )) {

        return false;
    }

    if (scheduler_fair_set_weight(&thread, 0)) {
        return false;
    }

    return thread.fair_weight == 2048;
}


/*
 * Verify accounting overflow is rejected without
 * partially modifying the thread.
 */
static bool scheduler_test_fair_overflow(void) {

    struct thread thread;

    scheduler_test_thread_init(
        &thread,
        28,
        THREAD_CLASS_FAIR,
        0,
        0,
        0
    );

    thread.runtime_ns = UINT64_MAX;

    if (scheduler_fair_account_runtime(&thread, 1)) {
        return false;
    }

    if (thread.runtime_ns != UINT64_MAX ||
        thread.virtual_runtime != 0) {

        return false;
    }

    /*
     * A deadline that cannot be represented must
     * also be rejected.
     */
    thread.runtime_ns = 0;
    thread.virtual_runtime = UINT64_MAX - 10;
    thread.fair_slice_ns = THREAD_FAIR_DEFAULT_SLICE_NS;

    if (scheduler_fair_update_deadline(&thread)) {
        return false;
    }

    return thread.virtual_runtime == UINT64_MAX - 10;
}

/*
 * Verify a thread cannot enter two queues simultaneously.
 */
static bool scheduler_test_duplicate_insertion(void) {

    struct scheduler_run_queue first_queue;
    struct scheduler_run_queue second_queue;

    struct thread thread;

    scheduler_run_queue_init(&first_queue);
    scheduler_run_queue_init(&second_queue);

    scheduler_test_thread_init(
        &thread,
        7,
        THREAD_CLASS_FAIR,
        0,
        100,
        100
    );

    if (!scheduler_run_queue_enqueue(
            &first_queue,
            &thread
        )) {

        return false;
    }

    if (scheduler_run_queue_enqueue(
            &second_queue,
            &thread
        )) {

        return false;
    }

    if (thread.run_queue_owner != &first_queue) {
        return false;
    }

    if (!scheduler_run_queue_dequeue(
            &first_queue,
            &thread
        )) {

        return false;
    }

    return thread.run_queue_owner == NULL &&
           first_queue.runnable_count == 0 &&
           second_queue.runnable_count == 0;
}


/*
 * Verify idle-class threads cannot enter a normal queue.
 */
static bool scheduler_test_idle_rejection(void) {

    struct scheduler_run_queue queue;

    struct thread idle;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &idle,
        8,
        THREAD_CLASS_IDLE,
        0,
        0,
        0
    );

    if (scheduler_run_queue_enqueue(&queue, &idle)) {
        return false;
    }

    return queue.runnable_count == 0 &&
           idle.run_queue_owner == NULL;
}


/*
 * Reject a thread with stale wait-queue metadata.
 */
static bool scheduler_test_stale_wait_membership(void) {

    struct scheduler_run_queue queue;
    struct thread thread;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &thread, 20, THREAD_CLASS_FAIR, 0, 100, 100
    );

    thread.wait_channel = (void *)&queue;

    if (scheduler_run_queue_enqueue(&queue, &thread)) {
        return false;
    }

    return queue.runnable_count == 0 &&
           thread.run_queue_owner == NULL &&
           thread.state == THREAD_STATE_NEW;
}


/*
 * Reject a thread with stale sleep-queue metadata.
 */
static bool scheduler_test_stale_sleep_membership(void) {

    struct scheduler_run_queue queue;
    struct thread thread;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &thread, 21, THREAD_CLASS_FAIR, 0, 100, 100
    );

    thread.sleep_deadline_ns = 1000;

    if (scheduler_run_queue_enqueue(&queue, &thread)) {
        return false;
    }

    return queue.runnable_count == 0 &&
           thread.run_queue_owner == NULL &&
           thread.state == THREAD_STATE_NEW;
}


/*
 * Reject a thread pending deferred reclamation.
 */
static bool scheduler_test_stale_reaper_membership(void) {

    struct scheduler_run_queue queue;
    struct thread thread;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &thread, 22, THREAD_CLASS_FAIR, 0, 100, 100
    );

    thread.reap_pending = true;

    if (scheduler_run_queue_enqueue(&queue, &thread)) {
        return false;
    }

    return queue.runnable_count == 0 &&
           thread.run_queue_owner == NULL &&
           thread.state == THREAD_STATE_NEW;
}


/*
 * Confirm a completely detached NEW thread remains admissible.
 */
static bool scheduler_test_detached_thread_admission(void) {

    struct scheduler_run_queue queue;
    struct thread thread;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &thread, 23, THREAD_CLASS_FAIR, 0, 100, 100
    );

    if (!scheduler_run_queue_enqueue(&queue, &thread)) {
        return false;
    }

    return queue.runnable_count == 1 &&
           thread.run_queue_owner == &queue &&
           thread.state == THREAD_STATE_READY;
}


/*
 * Verify queue removal preserves remaining links.
 */
static bool scheduler_test_middle_removal(void) {

    struct scheduler_run_queue queue;

    struct thread first;
    struct thread middle;
    struct thread last;

    scheduler_run_queue_init(&queue);

    scheduler_test_thread_init(
        &first, 9, THREAD_CLASS_FAIR, 0, 100, 100
    );

    scheduler_test_thread_init(
        &middle, 10, THREAD_CLASS_FAIR, 0, 200, 200
    );

    scheduler_test_thread_init(
        &last, 11, THREAD_CLASS_FAIR, 0, 300, 300
    );

    if (!scheduler_run_queue_enqueue(&queue, &first) ||
        !scheduler_run_queue_enqueue(&queue, &middle) ||
        !scheduler_run_queue_enqueue(&queue, &last)) {

        return false;
    }

    if (!scheduler_run_queue_dequeue(&queue, &middle)) {
        return false;
    }

    if (queue.runnable_count != 2 ||
        queue.fair.head != &first ||
        queue.fair.tail != &last ||
        first.run_next != &last ||
        last.run_previous != &first ||
        middle.run_queue_owner != NULL) {

        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &first) {
        return false;
    }

    if (scheduler_run_queue_pick_next(&queue) != &last) {
        return false;
    }

    return queue.runnable_count == 0;
}


/*
 * Verify CPU0 scheduler initialization.
 *
 * scheduler_init() is called by main before this test.
 */
static bool scheduler_test_cpu0(void) {

    if (!scheduler_is_initialized()) {
        return false;
    }

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        cpu->logical_id != 0 ||
        cpu->current_thread == NULL ||
        cpu->idle_thread == NULL) {

        return false;
    }

    if (cpu->current_thread->state != THREAD_STATE_RUNNING ||
        cpu->idle_thread->state != THREAD_STATE_READY) {

        return false;
    }

    if (cpu->current_thread == cpu->idle_thread) {
        return false;
    }

    if (cpu->current_thread->stack_owned ||
        !cpu->idle_thread->stack_owned ||
        cpu->current_thread->stack_base != NULL ||
        cpu->idle_thread->stack_base == NULL) {

        return false;
    }

    if (cpu->idle_thread->scheduling_class !=
        THREAD_CLASS_IDLE) {

        return false;
    }

    if (cpu->run_queue.runnable_count != 0) {
        return false;
    }

    if (scheduler_get_cpu(1) != NULL) {
        return false;
    }

    if (scheduler_get_current_thread(0) !=
        cpu->current_thread) {

        return false;
    }

    return true;
}


/*
 * Run scheduler core tests with individual failure diagnostics.
 */
bool scheduler_run_tests(void) {

#define RUN_SCHEDULER_TEST(name, test_function) \
    do { \
        if (!(test_function)()) { \
            console_write("[ FAILED ] " name "\n"); \
            return false; \
        } \
    } while (0)

    RUN_SCHEDULER_TEST(
        "queue initialization",
        scheduler_test_queue_init
    );

    RUN_SCHEDULER_TEST(
        "realtime FIFO",
        scheduler_test_realtime_fifo
    );

    RUN_SCHEDULER_TEST(
        "realtime priority",
        scheduler_test_realtime_priority
    );

    RUN_SCHEDULER_TEST(
        "fair deadline",
        scheduler_test_fair_deadline
    );

    RUN_SCHEDULER_TEST(
        "fair slice preemption",
        scheduler_test_fair_slice_preemption
    );

    RUN_SCHEDULER_TEST(
        "fair deadline preservation",
        scheduler_test_fair_deadline_preservation
    );

    RUN_SCHEDULER_TEST(
        "weighted runtime",
        scheduler_test_weighted_runtime
    );

    RUN_SCHEDULER_TEST(
        "weighted average",
        scheduler_test_weighted_average
    );

    RUN_SCHEDULER_TEST(
        "fair eligibility",
        scheduler_test_fair_eligibility
    );

    RUN_SCHEDULER_TEST(
        "fair weight validation",
        scheduler_test_fair_weight_validation
    );

    RUN_SCHEDULER_TEST(
        "fair overflow",
        scheduler_test_fair_overflow
    );

    RUN_SCHEDULER_TEST(
        "duplicate insertion",
        scheduler_test_duplicate_insertion
    );

    RUN_SCHEDULER_TEST(
        "idle rejection",
        scheduler_test_idle_rejection
    );

    RUN_SCHEDULER_TEST(
        "stale wait membership",
        scheduler_test_stale_wait_membership
    );

    RUN_SCHEDULER_TEST(
        "stale sleep membership",
        scheduler_test_stale_sleep_membership
    );

    RUN_SCHEDULER_TEST(
        "stale reaper membership",
        scheduler_test_stale_reaper_membership
    );

    RUN_SCHEDULER_TEST(
        "detached thread admission",
        scheduler_test_detached_thread_admission
    );

    RUN_SCHEDULER_TEST(
        "middle removal",
        scheduler_test_middle_removal
    );

    RUN_SCHEDULER_TEST(
        "CPU0 initialization",
        scheduler_test_cpu0
    );

#undef RUN_SCHEDULER_TEST

    return true;
}
