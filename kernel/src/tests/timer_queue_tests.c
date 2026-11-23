
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "time/timer_queue.h"
#include "time/time.h"
#include "tests/timer_queue_tests.h"


/* ============================================================
 * Test callback state
 * ============================================================ */

static uint64_t callback_count;

static uint64_t callback_order_count;

static uint32_t callback_order[3];


struct timer_test_context {
    uint32_t id;
};


static struct timer_test_context order_contexts[3] = {
    { .id = 1 },
    { .id = 2 },
    { .id = 3 }
};


static void timer_test_callback(void *context) {

    (void)context;

    callback_count++;
}


static void timer_order_callback(void *context) {

    struct timer_test_context *test =
        (struct timer_test_context *)context;

    if (test == NULL ||
        callback_order_count >= 3) {

        return;
    }

    callback_order[callback_order_count++] =
        test->id;
}


/* ============================================================
 * Time helpers
 * ============================================================ */

/*
 * Busy-wait using the HPET-backed monotonic clocksource.
 *
 * This is only for initialization-time tests.
 * It is not the implementation of a kernel sleep function.
 */
static bool timer_test_wait_ns(uint64_t duration_ns) {

    uint64_t start_ns = 0;

    if (time_get_monotonic_ns(&start_ns) !=
        TIME_STATUS_OK) {

        return false;
    }

    for (uint64_t attempts = 0;
         attempts < 100000000ULL;
         attempts++) {

        uint64_t now_ns = 0;

        if (time_get_monotonic_ns(&now_ns) !=
            TIME_STATUS_OK) {

            return false;
        }

        if (now_ns - start_ns >= duration_ns) {
            return true;
        }
    }

    return false;
}


/* ============================================================
 * Test helpers
 * ============================================================ */

static bool timer_test_expect(
    bool condition
) {

    return condition;
}


/* ============================================================
 * Initialization and argument tests
 * ============================================================ */

static bool timer_test_initialization(void) {

    if (!timer_test_expect(
            timer_queue_is_initialized())) {

        return false;
    }

    if (timer_queue_init() !=
        TIMER_QUEUE_STATUS_ALREADY_INITIALIZED) {

        return false;
    }

    timer_handle_t handle;

    if (timer_queue_schedule_after(
            0,
            timer_test_callback,
            NULL,
            &handle
        ) != TIMER_QUEUE_STATUS_INVALID_ARGUMENT) {

        return false;
    }

    if (timer_queue_schedule_after(
            1000000ULL,
            NULL,
            NULL,
            &handle
        ) != TIMER_QUEUE_STATUS_INVALID_ARGUMENT) {

        return false;
    }

    if (timer_queue_schedule_after(
            1000000ULL,
            timer_test_callback,
            NULL,
            NULL
        ) != TIMER_QUEUE_STATUS_INVALID_ARGUMENT) {

        return false;
    }

    if (timer_queue_schedule_periodic(
            0,
            timer_test_callback,
            NULL,
            &handle
        ) != TIMER_QUEUE_STATUS_INVALID_ARGUMENT) {

        return false;
    }

    return true;
}


/* ============================================================
 * One-shot timer tests
 * ============================================================ */

static bool timer_test_one_shot(void) {

    callback_count = 0;

    timer_handle_t handle;

    timer_queue_status_t status =
        timer_queue_schedule_after(
            5000000ULL,
            timer_test_callback,
            NULL,
            &handle
        );

    if (status != TIMER_QUEUE_STATUS_OK) {
        return false;
    }

    /*
     * Wait beyond the deadline.
     */
    if (!timer_test_wait_ns(10000000ULL)) {
        return false;
    }

    /*
     * Expiration detection must not execute callbacks.
     */
    if (timer_queue_poll() !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    if (callback_count != 0) {
        return false;
    }

    /*
     * Execute the deferred callback.
     */
    if (timer_queue_run_deferred(0) != 1) {
        return false;
    }

    if (callback_count != 1) {
        return false;
    }

    /*
     * The one-shot timer has already been released.
     */
    if (timer_queue_cancel(handle) !=
        TIMER_QUEUE_STATUS_INVALID_HANDLE) {

        return false;
    }

    return true;
}


/* ============================================================
 * Cancellation and stale handle tests
 * ============================================================ */

static bool timer_test_cancellation(void) {

    timer_handle_t old_handle;
    timer_handle_t new_handle;

    if (timer_queue_schedule_after(
            1000000000ULL,
            timer_test_callback,
            NULL,
            &old_handle
        ) != TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    if (timer_queue_cancel(old_handle) !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    /*
     * Reuse a timer slot.
     */
    if (timer_queue_schedule_after(
            1000000000ULL,
            timer_test_callback,
            NULL,
            &new_handle
        ) != TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    /*
     * Old handle must not cancel the new timer.
     */
    if (timer_queue_cancel(old_handle) !=
        TIMER_QUEUE_STATUS_INVALID_HANDLE) {

        return false;
    }

    if (timer_queue_cancel(new_handle) !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    return true;
}


/* ============================================================
 * Periodic timer tests
 * ============================================================ */

static bool timer_test_periodic(void) {

    callback_count = 0;

    timer_handle_t handle;

    if (timer_queue_schedule_periodic(
            3000000ULL,
            timer_test_callback,
            NULL,
            &handle
        ) != TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    /*
     * Wait past the first periodic expiration.
     */
    if (!timer_test_wait_ns(7000000ULL)) {
        return false;
    }

    if (timer_queue_poll() !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    if (callback_count != 0) {
        return false;
    }

    if (timer_queue_run_deferred(0) != 1) {
        return false;
    }

    if (callback_count != 1) {
        return false;
    }

    /*
     * The periodic timer should still be active.
     * Wait for another expiration.
     */
    if (!timer_test_wait_ns(7000000ULL)) {
        return false;
    }

    if (timer_queue_poll() !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    if (timer_queue_run_deferred(0) != 1) {
        return false;
    }

    if (callback_count != 2) {
        return false;
    }

    if (timer_queue_cancel(handle) !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    return true;
}


/* ============================================================
 * Deadline ordering tests
 * ============================================================ */

static bool timer_test_ordering(void) {

    callback_order_count = 0;

    for (size_t i = 0; i < 3; i++) {
        callback_order[i] = 0;
    }

    timer_handle_t handles[3];

    const uint64_t delays[3] = {
        5000000ULL,
        15000000ULL,
        25000000ULL
    };

    /*
     * Schedule timers with deliberately separated deadlines.
     */
    for (size_t i = 0; i < 3; i++) {

        if (timer_queue_schedule_after(
                delays[i],
                timer_order_callback,
                &order_contexts[i],
                &handles[i]
            ) != TIMER_QUEUE_STATUS_OK) {

            return false;
        }
    }

    /*
     * Wait until all three timers have expired.
     */
    if (!timer_test_wait_ns(35000000ULL)) {
        return false;
    }

    if (timer_queue_poll() !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    if (callback_order_count != 0) {
        return false;
    }

    if (timer_queue_run_deferred(0) != 3) {
        return false;
    }

    if (callback_order_count != 3) {
        return false;
    }

    /*
     * Verify callbacks were queued in deadline order.
     */
    if (callback_order[0] != 1 ||
        callback_order[1] != 2 ||
        callback_order[2] != 3) {

        return false;
    }

    return true;
}


/* ============================================================
 * Timer and callback capacity tests
 * ============================================================ */

static bool timer_test_capacity(void) {

    timer_handle_t handles[TIMER_QUEUE_MAX_TIMERS];

    callback_count = 0;

    /*
     * Fill every available timer slot.
     *
     * All timers use short deadlines so we can also
     * exercise the complete deferred callback capacity.
     */
    for (size_t i = 0;
         i < TIMER_QUEUE_MAX_TIMERS;
         i++) {

        if (timer_queue_schedule_after(
                5000000ULL,
                timer_test_callback,
                NULL,
                &handles[i]
            ) != TIMER_QUEUE_STATUS_OK) {

            return false;
        }
    }

    /*
     * The next allocation must fail.
     */
    timer_handle_t extra_handle;

    if (timer_queue_schedule_after(
            5000000ULL,
            timer_test_callback,
            NULL,
            &extra_handle
        ) != TIMER_QUEUE_STATUS_NO_TIMER_SLOTS) {

        return false;
    }

    /*
     * Wait until every scheduled timer has expired.
     */
    if (!timer_test_wait_ns(15000000ULL)) {
        return false;
    }

    if (timer_queue_poll() !=
        TIMER_QUEUE_STATUS_OK) {

        return false;
    }

    /*
     * No callbacks should have run in poll().
     */
    if (callback_count != 0) {
        return false;
    }

    /*
     * All 128 callback records should fit.
     */
    if (timer_queue_run_deferred(0) !=
        TIMER_QUEUE_MAX_TIMERS) {

        return false;
    }

    if (callback_count != TIMER_QUEUE_MAX_TIMERS) {
        return false;
    }

    /*
     * All one-shot timers should now be released.
     */
    timer_queue_stats_t stats;

    if (!timer_queue_get_stats(&stats)) {
        return false;
    }

    if (stats.active_timers != 0) {
        return false;
    }

    if (stats.callback_queue_overflows != 0) {
        return false;
    }

    return true;
}


/* ============================================================
 * Statistics tests
 * ============================================================ */

static bool timer_test_statistics(void) {

    timer_queue_stats_t stats;

    if (!timer_queue_get_stats(&stats)) {
        return false;
    }

    if (!stats.initialized) {
        return false;
    }

    if (stats.timers_created == 0 ||
        stats.timers_expired == 0 ||
        stats.callbacks_queued == 0 ||
        stats.callbacks_executed == 0 ||
        stats.poll_count == 0) {

        return false;
    }

    if (stats.callbacks_executed !=
        stats.callbacks_queued) {

        return false;
    }

    if (stats.active_timers != 0) {
        return false;
    }

    return true;
}


/* ============================================================
 * Public test entry point
 * ============================================================ */

bool timer_queue_run_tests(void) {

    if (!timer_test_initialization()) {
        return false;
    }

    if (!timer_test_one_shot()) {
        return false;
    }

    if (!timer_test_cancellation()) {
        return false;
    }

    if (!timer_test_periodic()) {
        return false;
    }

    if (!timer_test_ordering()) {
        return false;
    }

    if (!timer_test_capacity()) {
        return false;
    }

    if (!timer_test_statistics()) {
        return false;
    }

    return true;
}
