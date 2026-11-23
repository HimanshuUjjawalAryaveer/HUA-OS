#include <stdbool.h>
#include <stdint.h>

#include "tests/wait_queue_tests.h"

#include "scheduler/wait_queue.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"


/* ============================================================
 * Test helpers
 * ============================================================ */

static void wait_queue_test_thread_init(
    struct thread *thread,
    thread_id_t id,
    thread_state_t state
) {

    *thread = (struct thread){0};

    thread->id = id;
    thread->state = state;

    thread->scheduling_class = THREAD_CLASS_FAIR;

    thread->fair_weight =
        THREAD_FAIR_WEIGHT_DEFAULT;

    thread->fair_slice_ns =
        THREAD_FAIR_DEFAULT_SLICE_NS;

    thread->affinity_mask = 1ULL;
}


/* ============================================================
 * Initialization tests
 * ============================================================ */

static bool wait_queue_test_initialization(void) {

    struct wait_queue queue = {0};

    if (wait_queue_is_initialized(&queue)) {
        return false;
    }

    if (!wait_queue_init(&queue)) {
        return false;
    }

    if (!wait_queue_is_initialized(&queue)) {
        return false;
    }

    if (!wait_queue_is_empty(&queue)) {
        return false;
    }

    if (wait_queue_count(&queue) != 0) {
        return false;
    }

    if (!wait_queue_validate(&queue)) {
        return false;
    }

    /*
     * Reinitialization must be rejected.
     */
    if (wait_queue_init(&queue)) {
        return false;
    }

    return wait_queue_validate(&queue);
}


/* ============================================================
 * FIFO insertion and removal
 * ============================================================ */

static bool wait_queue_test_fifo(void) {

    struct wait_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;

    if (!wait_queue_init(&queue)) {
        return false;
    }

    wait_queue_test_thread_init(
        &first, 1, THREAD_STATE_BLOCKED
    );

    wait_queue_test_thread_init(
        &second, 2, THREAD_STATE_BLOCKED
    );

    wait_queue_test_thread_init(
        &third, 3, THREAD_STATE_BLOCKED
    );

    if (!wait_queue_enqueue(&queue, &first) ||
        !wait_queue_enqueue(&queue, &second) ||
        !wait_queue_enqueue(&queue, &third)) {

        return false;
    }

    /*
     * Verify FIFO ordering.
     */
    if (queue.head != &first ||
        queue.tail != &third ||
        queue.count != 3) {

        return false;
    }

    if (first.wait_previous != NULL ||
        first.wait_next != &second ||
        second.wait_previous != &first ||
        second.wait_next != &third ||
        third.wait_previous != &second ||
        third.wait_next != NULL) {

        return false;
    }

    if (first.wait_channel != &queue ||
        second.wait_channel != &queue ||
        third.wait_channel != &queue) {

        return false;
    }

    if (!wait_queue_validate(&queue)) {
        return false;
    }

    /*
     * Remove the middle thread.
     */
    if (!wait_queue_remove(&queue, &second)) {
        return false;
    }

    if (queue.head != &first ||
        queue.tail != &third ||
        queue.count != 2) {

        return false;
    }

    if (first.wait_next != &third ||
        third.wait_previous != &first) {

        return false;
    }

    if (second.wait_previous != NULL ||
        second.wait_next != NULL ||
        second.wait_channel != NULL) {

        return false;
    }

    /*
     * Remove the head.
     */
    if (!wait_queue_remove(&queue, &first)) {
        return false;
    }

    if (queue.head != &third ||
        queue.tail != &third ||
        queue.count != 1) {

        return false;
    }

    /*
     * Remove the final thread.
     */
    if (!wait_queue_remove(&queue, &third)) {
        return false;
    }

    return wait_queue_is_empty(&queue) &&
           wait_queue_count(&queue) == 0 &&
           wait_queue_validate(&queue);
}


/* ============================================================
 * Duplicate membership tests
 * ============================================================ */

static bool wait_queue_test_duplicate_membership(void) {

    struct wait_queue first_queue = {0};
    struct wait_queue second_queue = {0};

    struct thread thread;

    if (!wait_queue_init(&first_queue) ||
        !wait_queue_init(&second_queue)) {

        return false;
    }

    wait_queue_test_thread_init(
        &thread, 10, THREAD_STATE_BLOCKED
    );

    if (!wait_queue_enqueue(&first_queue, &thread)) {
        return false;
    }

    /*
     * The same thread cannot enter the same queue twice.
     */
    if (wait_queue_enqueue(&first_queue, &thread)) {
        return false;
    }

    /*
     * The same thread cannot enter another wait queue.
     */
    if (wait_queue_enqueue(&second_queue, &thread)) {
        return false;
    }

    if (wait_queue_count(&first_queue) != 1 ||
        wait_queue_count(&second_queue) != 0) {

        return false;
    }

    if (!wait_queue_validate(&first_queue) ||
        !wait_queue_validate(&second_queue)) {

        return false;
    }

    return wait_queue_remove(&first_queue, &thread);
}


/* ============================================================
 * Thread-state validation
 * ============================================================ */

static bool wait_queue_test_invalid_states(void) {

    struct wait_queue queue = {0};

    if (!wait_queue_init(&queue)) {
        return false;
    }

    const thread_state_t invalid_states[] = {
        THREAD_STATE_NEW,
        THREAD_STATE_READY,
        THREAD_STATE_RUNNING,
        THREAD_STATE_SLEEPING,
        THREAD_STATE_TERMINATED
    };

    for (size_t i = 0;
         i < sizeof(invalid_states) /
             sizeof(invalid_states[0]);
         i++) {

        struct thread thread;

        wait_queue_test_thread_init(
            &thread,
            20 + i,
            invalid_states[i]
        );

        if (wait_queue_enqueue(&queue, &thread)) {
            return false;
        }

        if (thread.wait_channel != NULL ||
            thread.wait_previous != NULL ||
            thread.wait_next != NULL) {

            return false;
        }
    }

    return wait_queue_is_empty(&queue) &&
           wait_queue_validate(&queue);
}


/* ============================================================
 * Runnable-thread exclusion
 * ============================================================ */

static bool wait_queue_test_run_queue_exclusion(void) {

    struct wait_queue queue = {0};

    struct scheduler_run_queue fake_run_queue = {0};

    struct thread thread;
    struct thread neighbor;

    if (!wait_queue_init(&queue)) {
        return false;
    }

    wait_queue_test_thread_init(
        &thread, 30, THREAD_STATE_BLOCKED
    );

    /*
     * A thread with run-queue ownership is invalid.
     */
    thread.run_queue_owner = &fake_run_queue;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.run_queue_owner = NULL;

    /*
     * Stale run-queue links must also be rejected.
     */
    wait_queue_test_thread_init(
        &neighbor, 31, THREAD_STATE_BLOCKED
    );

    thread.run_next = &neighbor;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.run_next = NULL;

    thread.run_previous = &neighbor;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.run_previous = NULL;

    return wait_queue_is_empty(&queue);
}


/* ============================================================
 * Reaper exclusion
 * ============================================================ */

static bool wait_queue_test_reaper_exclusion(void) {

    struct wait_queue queue = {0};

    struct thread thread;

    if (!wait_queue_init(&queue)) {
        return false;
    }

    wait_queue_test_thread_init(
        &thread, 40, THREAD_STATE_BLOCKED
    );

    thread.reap_pending = true;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.reap_pending = false;
    thread.reap_queued = true;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.reap_queued = false;

    return wait_queue_is_empty(&queue);
}


/* ============================================================
 * Sleep-queue exclusion
 * ============================================================ */

static bool wait_queue_test_sleep_queue_exclusion(void) {

    struct wait_queue queue = {0};
    struct thread thread;

    if (!wait_queue_init(&queue)) {
        return false;
    }

    wait_queue_test_thread_init(
        &thread, 45, THREAD_STATE_BLOCKED
    );

    /*
     * Reject stale sleep-queue previous link.
     */
    thread.sleep_previous = &thread;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.sleep_previous = NULL;

    /*
     * Reject stale sleep-queue next link.
     */
    thread.sleep_next = &thread;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.sleep_next = NULL;

    /*
     * Reject an active sleep-queue membership flag.
     */
    thread.sleep_queued = true;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.sleep_queued = false;

    /*
     * Reject a stale sleep deadline.
     */
    thread.sleep_deadline_ns = 1000;

    if (wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    thread.sleep_deadline_ns = 0;

    return wait_queue_is_empty(&queue) &&
           wait_queue_validate(&queue);
}


/* ============================================================
 * Sleep metadata corruption detection
 * ============================================================ */

static bool wait_queue_test_sleep_metadata_corruption(void) {

    struct wait_queue queue = {0};
    struct thread thread;

    if (!wait_queue_init(&queue)) {
        return false;
    }

    wait_queue_test_thread_init(
        &thread, 46, THREAD_STATE_BLOCKED
    );

    if (!wait_queue_enqueue(&queue, &thread)) {
        return false;
    }

    /*
     * Simulate a thread incorrectly appearing in both
     * a wait queue and a sleep queue.
     */
    thread.sleep_queued = true;

    if (wait_queue_validate(&queue)) {
        return false;
    }

    /*
     * Restore the synthetic corruption.
     */
    thread.sleep_queued = false;

    return wait_queue_validate(&queue) &&
           wait_queue_remove(&queue, &thread) &&
           wait_queue_is_empty(&queue);
}


/* ============================================================
 * Invalid removal tests
 * ============================================================ */

static bool wait_queue_test_invalid_removal(void) {

    struct wait_queue first_queue = {0};
    struct wait_queue second_queue = {0};

    struct thread first;
    struct thread second;

    if (!wait_queue_init(&first_queue) ||
        !wait_queue_init(&second_queue)) {

        return false;
    }

    wait_queue_test_thread_init(
        &first, 50, THREAD_STATE_BLOCKED
    );

    wait_queue_test_thread_init(
        &second, 51, THREAD_STATE_BLOCKED
    );

    if (!wait_queue_enqueue(&first_queue, &first)) {
        return false;
    }

    /*
     * A thread cannot be removed from the wrong queue.
     */
    if (wait_queue_remove(&second_queue, &first)) {
        return false;
    }

    /*
     * A thread that is not a member cannot be removed.
     */
    if (wait_queue_remove(&first_queue, &second)) {
        return false;
    }

    if (wait_queue_remove(NULL, &first) ||
        wait_queue_remove(&first_queue, NULL)) {

        return false;
    }

    if (wait_queue_count(&first_queue) != 1 ||
        first.wait_channel != &first_queue) {

        return false;
    }

    return wait_queue_remove(&first_queue, &first) &&
           wait_queue_is_empty(&first_queue);
}


/* ============================================================
 * Corruption detection
 * ============================================================ */

static bool wait_queue_test_corruption_detection(void) {

    struct wait_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;

    if (!wait_queue_init(&queue)) {
        return false;
    }

    wait_queue_test_thread_init(
        &first, 60, THREAD_STATE_BLOCKED
    );

    wait_queue_test_thread_init(
        &second, 61, THREAD_STATE_BLOCKED
    );

    wait_queue_test_thread_init(
        &third, 62, THREAD_STATE_BLOCKED
    );

    if (!wait_queue_enqueue(&queue, &first) ||
        !wait_queue_enqueue(&queue, &second)) {

        return false;
    }

    /*
     * Simulate a damaged previous link.
     */
    second.wait_previous = NULL;

    if (wait_queue_validate(&queue)) {
        return false;
    }

    /*
     * Mutating operations must refuse a corrupted queue.
     */
    if (wait_queue_enqueue(&queue, &third)) {
        return false;
    }

    if (wait_queue_remove(&queue, &first)) {
        return false;
    }

    /*
     * Repair the synthetic corruption.
     */
    second.wait_previous = &first;

    if (!wait_queue_validate(&queue)) {
        return false;
    }

    if (!wait_queue_remove(&queue, &first) ||
        !wait_queue_remove(&queue, &second)) {

        return false;
    }

    return wait_queue_validate(&queue) &&
           wait_queue_is_empty(&queue);
}

/* ============================================================
 * Scheduler-integrated wakeup tests
 * ============================================================ */

static bool wait_queue_test_wakeup(void) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL || !cpu->initialized) {
        return false;
    }

    struct wait_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;

    if (!wait_queue_init(&queue)) {
        return false;
    }

    wait_queue_test_thread_init(
        &first, 70, THREAD_STATE_BLOCKED
    );

    wait_queue_test_thread_init(
        &second, 71, THREAD_STATE_BLOCKED
    );

    wait_queue_test_thread_init(
        &third, 72, THREAD_STATE_BLOCKED
    );

    if (!wait_queue_enqueue(&queue, &first) ||
        !wait_queue_enqueue(&queue, &second) ||
        !wait_queue_enqueue(&queue, &third)) {

        return false;
    }

    /*
     * Wake exactly the first waiter.
     */
    if (!wait_queue_wake_one(&queue)) {
        return false;
    }

    if (first.state != THREAD_STATE_READY ||
        first.wait_channel != NULL ||
        first.run_queue_owner != &cpu->run_queue ||
        wait_queue_count(&queue) != 2) {

        return false;
    }

    /*
     * A second wake of the same thread must fail.
     */
    if (thread_wake(&first)) {
        return false;
    }

    /*
     * Wake all remaining threads.
     */
    uint64_t awakened =
        wait_queue_wake_all(&queue);

    if (awakened != 2 ||
        !wait_queue_is_empty(&queue)) {

        return false;
    }

    if (second.state != THREAD_STATE_READY ||
        third.state != THREAD_STATE_READY ||
        second.run_queue_owner != &cpu->run_queue ||
        third.run_queue_owner != &cpu->run_queue) {

        return false;
    }

    /*
     * An empty queue cannot wake a thread.
     */
    if (wait_queue_wake_one(&queue) ||
        wait_queue_wake_all(&queue) != 0) {

        return false;
    }

    /*
     * Remove our synthetic threads from the real run queue.
     * These test threads have no allocated stacks and are
     * never dispatched.
     */
    if (!scheduler_run_queue_dequeue(
            &cpu->run_queue,
            &first
        )) {

        return false;
    }

    if (!scheduler_run_queue_dequeue(
            &cpu->run_queue,
            &second
        )) {

        return false;
    }

    if (!scheduler_run_queue_dequeue(
            &cpu->run_queue,
            &third
        )) {

        return false;
    }

    if (first.run_queue_owner != NULL ||
        second.run_queue_owner != NULL ||
        third.run_queue_owner != NULL) {

        return false;
    }

    return wait_queue_validate(&queue);
}


/* ============================================================
 * Complete test suite
 * ============================================================ */

bool wait_queue_run_tests(void) {

    if (!wait_queue_test_initialization()) {
        return false;
    }

    if (!wait_queue_test_fifo()) {
        return false;
    }

    if (!wait_queue_test_duplicate_membership()) {
        return false;
    }

    if (!wait_queue_test_invalid_states()) {
        return false;
    }

    if (!wait_queue_test_run_queue_exclusion()) {
        return false;
    }

    if (!wait_queue_test_reaper_exclusion()) {
        return false;
    }

    if (!wait_queue_test_sleep_queue_exclusion()) {
        return false;
    }

    if (!wait_queue_test_sleep_metadata_corruption()) {
        return false;
    }

    if (!wait_queue_test_invalid_removal()) {
        return false;
    }

    if (!wait_queue_test_corruption_detection()) {
        return false;
    }

    if (!wait_queue_test_wakeup()) {
        return false;
    }

    return true;
}
