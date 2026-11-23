#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "tests/sleep_queue_tests.h"

#include "scheduler/sleep_queue.h"
#include "scheduler/thread.h"


/* ============================================================
 * Test helpers
 * ============================================================ */

static void sleep_queue_test_thread_init(
    struct thread *thread,
    thread_id_t id,
    uint64_t deadline_ns,
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

    thread->sleep_deadline_ns = deadline_ns;
}


/* ============================================================
 * Initialization tests
 * ============================================================ */

static bool sleep_queue_test_initialization(void) {

    struct scheduler_sleep_queue queue = {0};

    sleep_queue_init(&queue);

    if (queue.head != NULL ||
        queue.tail != NULL ||
        queue.count != 0) {

        return false;
    }

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    if (sleep_queue_count(&queue) != 0) {
        return false;
    }

    return true;
}


/* ============================================================
 * Deadline ordering tests
 * ============================================================ */

static bool sleep_queue_test_deadline_ordering(void) {

    struct scheduler_sleep_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;
    struct thread fourth;

    sleep_queue_init(&queue);

    sleep_queue_test_thread_init(
        &first, 1, 300, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &second, 2, 100, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &third, 3, 400, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &fourth, 4, 200, THREAD_STATE_SLEEPING
    );

    /*
     * Insert in deliberately unsorted order.
     */
    if (!sleep_queue_insert(&queue, &first) ||
        !sleep_queue_insert(&queue, &second) ||
        !sleep_queue_insert(&queue, &third) ||
        !sleep_queue_insert(&queue, &fourth)) {

        return false;
    }

    /*
     * Expected order:
     * second (100)
     * fourth (200)
     * first (300)
     * third (400)
     */
    if (queue.head != &second ||
        queue.tail != &third ||
        queue.count != 4) {

        return false;
    }

    if (second.sleep_next != &fourth ||
        fourth.sleep_next != &first ||
        first.sleep_next != &third ||
        third.sleep_next != NULL) {

        return false;
    }

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    return sleep_queue_count(&queue) == 4;
}


/* ============================================================
 * Equal-deadline FIFO tests
 * ============================================================ */

static bool sleep_queue_test_equal_deadlines(void) {

    struct scheduler_sleep_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;

    sleep_queue_init(&queue);

    sleep_queue_test_thread_init(
        &first, 10, 500, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &second, 11, 500, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &third, 12, 500, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &first) ||
        !sleep_queue_insert(&queue, &second) ||
        !sleep_queue_insert(&queue, &third)) {

        return false;
    }

    /*
     * Equal deadlines retain insertion order.
     */
    if (queue.head != &first ||
        queue.tail != &third ||
        first.sleep_next != &second ||
        second.sleep_next != &third) {

        return false;
    }

    return sleep_queue_validate(&queue);
}


/* ============================================================
 * Removal tests
 * ============================================================ */

static bool sleep_queue_test_removal(void) {

    struct scheduler_sleep_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;

    sleep_queue_init(&queue);

    sleep_queue_test_thread_init(
        &first, 20, 100, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &second, 21, 200, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &third, 22, 300, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &first) ||
        !sleep_queue_insert(&queue, &second) ||
        !sleep_queue_insert(&queue, &third)) {

        return false;
    }

    /*
     * Remove the middle thread.
     */
    if (!sleep_queue_remove(&queue, &second)) {
        return false;
    }

    if (queue.head != &first ||
        queue.tail != &third ||
        queue.count != 2 ||
        first.sleep_next != &third ||
        third.sleep_previous != &first) {

        return false;
    }

    if (second.sleep_queued ||
        second.sleep_previous != NULL ||
        second.sleep_next != NULL ||
        second.sleep_deadline_ns != 0) {

        return false;
    }

    /*
     * Remove the head.
     */
    if (!sleep_queue_remove(&queue, &first)) {
        return false;
    }

    if (queue.head != &third ||
        queue.tail != &third ||
        queue.count != 1) {

        return false;
    }

    /*
     * Remove the tail.
     */
    if (!sleep_queue_remove(&queue, &third)) {
        return false;
    }

    return queue.head == NULL &&
           queue.tail == NULL &&
           queue.count == 0 &&
           sleep_queue_validate(&queue);
}


/* ============================================================
 * Expiration tests
 * ============================================================ */

static bool sleep_queue_test_expiration(void) {

    struct scheduler_sleep_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;

    sleep_queue_init(&queue);

    sleep_queue_test_thread_init(
        &first, 30, 100, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &second, 31, 200, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &third, 32, 300, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &first) ||
        !sleep_queue_insert(&queue, &second) ||
        !sleep_queue_insert(&queue, &third)) {

        return false;
    }

    uint64_t expired_deadline = UINT64_MAX;

    /*
     * Nothing expires before the first deadline.
     * The output parameter must remain unchanged.
     */
    if (sleep_queue_pop_expired(
            &queue, 99, &expired_deadline) != NULL ||
        expired_deadline != UINT64_MAX) {

        return false;
    }

    /*
     * A deadline equal to now is expired.
     */
    struct thread *expired = sleep_queue_pop_expired(
        &queue, 100, &expired_deadline
    );

    if (expired != &first ||
        expired_deadline != 100) {

        return false;
    }

    /*
     * Expiration must complete the transition to READY.
     */
    if (first.sleep_queued ||
        first.sleep_previous != NULL ||
        first.sleep_next != NULL ||
        first.sleep_deadline_ns != 0 ||
        first.state != THREAD_STATE_READY) {

        return false;
    }

    if (queue.head != &second ||
        queue.count != 2) {

        return false;
    }

    /*
     * An already-expired thread must not be returned again.
     */
    if (sleep_queue_pop_expired(
            &queue, 100, &expired_deadline) != NULL) {

        return false;
    }

    /*
     * The second deadline has not expired yet.
     */
    if (sleep_queue_pop_expired(
            &queue, 199, &expired_deadline) != NULL) {

        return false;
    }

    expired = sleep_queue_pop_expired(
        &queue, 250, &expired_deadline
    );

    if (expired != &second ||
        expired_deadline != 200 ||
        second.state != THREAD_STATE_READY ||
        second.sleep_queued ||
        second.sleep_previous != NULL ||
        second.sleep_next != NULL ||
        second.sleep_deadline_ns != 0) {

        return false;
    }

    expired = sleep_queue_pop_expired(
        &queue, 300, &expired_deadline
    );

    if (expired != &third ||
        expired_deadline != 300 ||
        third.state != THREAD_STATE_READY ||
        third.sleep_queued ||
        third.sleep_previous != NULL ||
        third.sleep_next != NULL ||
        third.sleep_deadline_ns != 0) {

        return false;
    }

    return queue.head == NULL &&
           queue.tail == NULL &&
           queue.count == 0 &&
           sleep_queue_validate(&queue);
}

/* ============================================================
 * Invalid-state and membership tests
 * ============================================================ */

static bool sleep_queue_test_invalid_threads(void) {

    struct scheduler_sleep_queue queue = {0};

    sleep_queue_init(&queue);

    const thread_state_t invalid_states[] = {
        THREAD_STATE_NEW,
        THREAD_STATE_READY,
        THREAD_STATE_RUNNING,
        THREAD_STATE_BLOCKED,
        THREAD_STATE_TERMINATED
    };

    for (size_t i = 0;
         i < sizeof(invalid_states) /
             sizeof(invalid_states[0]);
         i++) {

        struct thread thread;

        sleep_queue_test_thread_init(
            &thread,
            40 + i,
            1000,
            invalid_states[i]
        );

        if (sleep_queue_insert(&queue, &thread)) {
            return false;
        }

        if (thread.sleep_queued ||
            thread.sleep_previous != NULL ||
            thread.sleep_next != NULL) {

            return false;
        }
    }

    /*
     * A zero deadline is invalid for a queued sleeper.
     */
    struct thread zero_deadline;

    sleep_queue_test_thread_init(
        &zero_deadline,
        50,
        0,
        THREAD_STATE_SLEEPING
    );

    if (sleep_queue_insert(&queue, &zero_deadline)) {
        return false;
    }

    return queue.count == 0 &&
           sleep_queue_validate(&queue);
}


/* ============================================================
 * Duplicate membership and exclusion tests
 * ============================================================ */

static bool sleep_queue_test_membership_exclusion(void) {

    struct scheduler_sleep_queue first_queue = {0};
    struct scheduler_sleep_queue second_queue = {0};

    struct thread thread;

    sleep_queue_init(&first_queue);
    sleep_queue_init(&second_queue);

    sleep_queue_test_thread_init(
        &thread,
        60,
        1000,
        THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&first_queue, &thread)) {
        return false;
    }

    /*
     * Duplicate insertion into the same queue is invalid.
     */
    if (sleep_queue_insert(&first_queue, &thread)) {
        return false;
    }

    /*
     * A thread cannot belong to two sleep queues.
     */
    if (sleep_queue_insert(&second_queue, &thread)) {
        return false;
    }

    /*
     * A sleeping thread cannot simultaneously be blocked
     * on an ordinary wait queue.
     */
    thread.wait_channel = (void *)1;

    if (sleep_queue_validate(&first_queue)) {
        return false;
    }

    thread.wait_channel = NULL;

    if (!sleep_queue_validate(&first_queue)) {
        return false;
    }

    /*
     * A thread with stale run-queue linkage is invalid.
     */
    struct thread neighbor = {0};

    thread.run_next = &neighbor;

    if (sleep_queue_validate(&first_queue)) {
        return false;
    }

    thread.run_next = NULL;

    if (!sleep_queue_validate(&first_queue)) {
        return false;
    }

    /*
     * A thread pending reclamation cannot be sleeping.
     */
    thread.reap_pending = true;

    if (sleep_queue_validate(&first_queue)) {
        return false;
    }

    thread.reap_pending = false;

    if (!sleep_queue_validate(&first_queue)) {
        return false;
    }

    return sleep_queue_remove(&first_queue, &thread) &&
           sleep_queue_validate(&first_queue) &&
           sleep_queue_validate(&second_queue);
}


/* ============================================================
 * Invalid removal tests
 * ============================================================ */

static bool sleep_queue_test_invalid_removal(void) {

    struct scheduler_sleep_queue first_queue = {0};
    struct scheduler_sleep_queue second_queue = {0};

    struct thread first;
    struct thread second;

    sleep_queue_init(&first_queue);
    sleep_queue_init(&second_queue);

    sleep_queue_test_thread_init(
        &first, 70, 100, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &second, 71, 200, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&first_queue, &first)) {
        return false;
    }

    /*
     * Wrong queue.
     */
    if (sleep_queue_remove(&second_queue, &first)) {
        return false;
    }

    /*
     * Thread is not a member.
     */
    if (sleep_queue_remove(&first_queue, &second)) {
        return false;
    }

    /*
     * Invalid arguments.
     */
    if (sleep_queue_remove(NULL, &first) ||
        sleep_queue_remove(&first_queue, NULL)) {

        return false;
    }

    if (first_queue.count != 1 ||
        first.sleep_queued != true) {

        return false;
    }

    return sleep_queue_remove(&first_queue, &first) &&
           sleep_queue_validate(&first_queue);
}


/* ============================================================
 * O(1) expiration fast-path validation tests
 * ============================================================ */

static bool sleep_queue_test_expiration_fast_path_validation(void) {

    struct scheduler_sleep_queue queue = {0};

    struct thread first;
    struct thread second;

    uint64_t expired_deadline = 0;

    sleep_queue_init(&queue);

    /*
     * Test 1: Reject a corrupted head.
     */
    sleep_queue_test_thread_init(
        &first, 90, 100, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &first)) {
        return false;
    }

    first.sleep_queued = false;

    if (sleep_queue_pop_expired(
            &queue, 100, &expired_deadline) != NULL) {
        return false;
    }

    first.sleep_queued = true;

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    /*
     * Test 2: Reject a corrupted immediate successor.
     */
    sleep_queue_test_thread_init(
        &second, 91, 200, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &second)) {
        return false;
    }

    second.sleep_previous = NULL;

    if (sleep_queue_pop_expired(
            &queue, 100, &expired_deadline) != NULL) {
        return false;
    }

    second.sleep_previous = &first;

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    /*
     * Test 3: Reject a successor with an invalid state.
     */
    second.state = THREAD_STATE_READY;

    if (sleep_queue_pop_expired(
            &queue, 100, &expired_deadline) != NULL) {
        return false;
    }

    second.state = THREAD_STATE_SLEEPING;

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    /*
     * Test 4: Reject a successor whose deadline
     * violates the queue's ordering.
     */
    second.sleep_deadline_ns = 50;

    if (sleep_queue_pop_expired(
            &queue, 100, &expired_deadline) != NULL) {
        return false;
    }

    second.sleep_deadline_ns = 200;

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    /*
     * Test 5: Reject an inconsistent single-element
     * queue whose count does not match its endpoints.
     */
    if (!sleep_queue_remove(&queue, &second)) {
        return false;
    }

    queue.count = 2;

    if (sleep_queue_pop_expired(
            &queue, 100, &expired_deadline) != NULL) {
        return false;
    }

    queue.count = 1;

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    /*
     * Final valid expiration must still succeed.
     */
    struct thread *expired = sleep_queue_pop_expired(
        &queue, 100, &expired_deadline
    );

    return expired == &first &&
           expired_deadline == 100 &&
           first.state == THREAD_STATE_READY &&
           !first.sleep_queued &&
           first.sleep_previous == NULL &&
           first.sleep_next == NULL &&
           first.sleep_deadline_ns == 0 &&
           sleep_queue_validate(&queue);
}

/* ============================================================
 * Corruption detection
 * ============================================================ */

static bool sleep_queue_test_corruption_detection(void) {

    struct scheduler_sleep_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;

    sleep_queue_init(&queue);

    sleep_queue_test_thread_init(
        &first, 80, 100, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &second, 81, 200, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &third, 82, 300, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &first) ||
        !sleep_queue_insert(&queue, &second)) {

        return false;
    }

    /*
     * Damage the previous link.
     */
    second.sleep_previous = NULL;

    if (sleep_queue_validate(&queue)) {
        return false;
    }

    uint64_t expired_deadline = 0;

    /*
     * Mutating operations must reject the damaged queue.
     */
    if (sleep_queue_insert(&queue, &third) ||
        sleep_queue_remove(&queue, &first) ||
        sleep_queue_pop_expired(&queue, 1000, &expired_deadline) != NULL) {

        return false;
    }

    /*
     * Repair the synthetic corruption.
     */
    second.sleep_previous = &first;

    if (!sleep_queue_validate(&queue)) {
        return false;
    }

    if (!sleep_queue_remove(&queue, &first) ||
        !sleep_queue_remove(&queue, &second)) {

        return false;
    }

    return sleep_queue_validate(&queue) &&
           sleep_queue_count(&queue) == 0;
}


/* ============================================================
 * Expiration rollback tests
 * ============================================================ */

static bool sleep_queue_test_expiration_rollback(void) {

    struct scheduler_sleep_queue queue = {0};

    struct thread first;
    struct thread second;
    struct thread third;
    struct thread invalid;

    uint64_t expired_deadline = 0;

    sleep_queue_init(&queue);

    /*
     * Test 1:
     * Restore an expired thread into an empty queue.
     */
    sleep_queue_test_thread_init(
        &first, 90, 100, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &first)) {
        return false;
    }

    struct thread *expired =
        sleep_queue_pop_expired(
            &queue, 100, &expired_deadline
        );

    if (expired != &first ||
        expired_deadline != 100 ||
        first.state != THREAD_STATE_READY) {
        return false;
    }

    if (!sleep_queue_restore_expired(
            &queue, &first, expired_deadline)) {
        return false;
    }

    if (queue.head != &first ||
        queue.tail != &first ||
        queue.count != 1 ||
        first.state != THREAD_STATE_SLEEPING ||
        !first.sleep_queued ||
        first.sleep_deadline_ns != 100 ||
        first.sleep_previous != NULL ||
        first.sleep_next != NULL ||
        !sleep_queue_validate(&queue)) {
        return false;
    }

    /*
     * Pop the thread again for the next scenarios.
     */
    expired = sleep_queue_pop_expired(
        &queue, 100, &expired_deadline
    );

    if (expired != &first || expired_deadline != 100) {
        return false;
    }

    /*
     * Test 2:
     * Restore before a thread with a later deadline.
     */
    sleep_queue_test_thread_init(
        &second, 91, 200, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &second) ||
        !sleep_queue_restore_expired(
            &queue, &first, 100)) {
        return false;
    }

    if (queue.head != &first ||
        queue.tail != &second ||
        queue.count != 2 ||
        first.sleep_next != &second ||
        second.sleep_previous != &first ||
        !sleep_queue_validate(&queue)) {
        return false;
    }

    if (!sleep_queue_remove(&queue, &first) ||
        !sleep_queue_remove(&queue, &second)) {
        return false;
    }

    /*
     * Test 3:
     * Restore before an existing equal-deadline thread.
     * The restored thread must retain FIFO precedence.
     */
    sleep_queue_test_thread_init(
        &first, 92, 300, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &second, 93, 300, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &third, 94, 400, THREAD_STATE_SLEEPING
    );

    if (!sleep_queue_insert(&queue, &first) ||
        !sleep_queue_insert(&queue, &second) ||
        !sleep_queue_insert(&queue, &third)) {
        return false;
    }

    expired = sleep_queue_pop_expired(
        &queue, 300, &expired_deadline
    );

    if (expired != &first || expired_deadline != 300) {
        return false;
    }

    if (!sleep_queue_restore_expired(
            &queue, &first, expired_deadline)) {
        return false;
    }

    if (queue.head != &first ||
        first.sleep_next != &second ||
        second.sleep_previous != &first ||
        second.sleep_next != &third ||
        third.sleep_previous != &second ||
        queue.tail != &third ||
        queue.count != 3 ||
        !sleep_queue_validate(&queue)) {
        return false;
    }

    if (!sleep_queue_remove(&queue, &first) ||
        !sleep_queue_remove(&queue, &second) ||
        !sleep_queue_remove(&queue, &third)) {
        return false;
    }

    /*
     * Test 4:
     * Reject a restoration that would violate
     * deadline ordering.
     */
    sleep_queue_test_thread_init(
        &second, 95, 100, THREAD_STATE_SLEEPING
    );

    sleep_queue_test_thread_init(
        &invalid, 96, 0, THREAD_STATE_READY
    );

    if (!sleep_queue_insert(&queue, &second)) {
        return false;
    }

    if (sleep_queue_restore_expired(
            &queue, &invalid, 200)) {
        return false;
    }

    if (queue.head != &second ||
        queue.tail != &second ||
        queue.count != 1 ||
        invalid.state != THREAD_STATE_READY ||
        invalid.sleep_queued ||
        invalid.sleep_deadline_ns != 0 ||
        !sleep_queue_validate(&queue)) {
        return false;
    }

    if (!sleep_queue_remove(&queue, &second)) {
        return false;
    }

    /*
     * Test 5a:
     * Reject a thread that is not READY.
     */
    sleep_queue_test_thread_init(
        &invalid, 97, 0, THREAD_STATE_BLOCKED
    );

    if (sleep_queue_restore_expired(
            &queue, &invalid, 200)) {
        return false;
    }

    if (queue.count != 0 ||
        invalid.state != THREAD_STATE_BLOCKED ||
        !sleep_queue_validate(&queue)) {
        return false;
    }

    /*
     * Test 5b:
     * Reject a READY thread with stale sleep membership.
     */
    sleep_queue_test_thread_init(
        &invalid, 98, 0, THREAD_STATE_READY
    );

    invalid.sleep_queued = true;

    if (sleep_queue_restore_expired(
            &queue, &invalid, 200)) {
        return false;
    }

    if (queue.count != 0 ||
        invalid.state != THREAD_STATE_READY ||
        !invalid.sleep_queued ||
        !sleep_queue_validate(&queue)) {
        return false;
    }

    return queue.head == NULL &&
           queue.tail == NULL &&
           queue.count == 0;
}


/* ============================================================
 * Complete test suite
 * ============================================================ */

bool sleep_queue_run_tests(void) {

    if (!sleep_queue_test_initialization()) {
        return false;
    }

    if (!sleep_queue_test_deadline_ordering()) {
        return false;
    }

    if (!sleep_queue_test_equal_deadlines()) {
        return false;
    }

    if (!sleep_queue_test_removal()) {
        return false;
    }

    if (!sleep_queue_test_expiration()) {
        return false;
    }

    if (!sleep_queue_test_invalid_threads()) {
        return false;
    }

    if (!sleep_queue_test_membership_exclusion()) {
        return false;
    }

    if (!sleep_queue_test_invalid_removal()) {
        return false;
    }

    if (!sleep_queue_test_expiration_fast_path_validation()) {
        return false;
    }

    if (!sleep_queue_test_expiration_rollback()) {
        return false;
    }

    if (!sleep_queue_test_corruption_detection()) {
        return false;
    }

    return true;
}
