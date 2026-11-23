
#ifndef HUA_TIMER_QUEUE_H
#define HUA_TIMER_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Fixed capacities.
 *
 * Timer metadata and deferred callback storage are static.
 * No heap allocation is required by the timer subsystem.
 */
#define TIMER_QUEUE_MAX_TIMERS       128U
#define TIMER_QUEUE_MAX_CALLBACKS    128U

/*
 * Invalid timer slot.
 */
#define TIMER_QUEUE_INVALID_SLOT     UINT16_MAX

/*
 * Timer handle.
 *
 * The generation prevents a stale handle from referring
 * to a new timer that happens to reuse the same slot.
 */
typedef struct {
    uint16_t slot;
    uint32_t generation;
} timer_handle_t;

/*
 * Timer callback.
 *
 * Callbacks execute in normal kernel context through
 * timer_queue_run_deferred(), never in the LAPIC IRQ handler.
 */
typedef void (*timer_callback_t)(void *context);

/*
 * Timer queue status codes.
 */
typedef enum {
    TIMER_QUEUE_STATUS_OK = 0,

    TIMER_QUEUE_STATUS_NOT_INITIALIZED,
    TIMER_QUEUE_STATUS_ALREADY_INITIALIZED,

    TIMER_QUEUE_STATUS_INVALID_ARGUMENT,
    TIMER_QUEUE_STATUS_INVALID_HANDLE,

    TIMER_QUEUE_STATUS_NO_TIMER_SLOTS,
    TIMER_QUEUE_STATUS_CALLBACK_QUEUE_FULL,

    TIMER_QUEUE_STATUS_TIME_FAILURE,
    TIMER_QUEUE_STATUS_TIME_OVERFLOW,

    TIMER_QUEUE_STATUS_INTERNAL_ERROR

} timer_queue_status_t;

/*
 * Public timer queue statistics.
 */
typedef struct {

    bool initialized;

    uint64_t active_timers;
    uint64_t peak_active_timers;

    uint64_t timers_created;
    uint64_t timers_cancelled;

    uint64_t timers_expired;
    uint64_t periodic_expirations;

    uint64_t callbacks_queued;
    uint64_t callbacks_executed;

    uint64_t callback_queue_overflows;

    uint64_t poll_count;

} timer_queue_stats_t;


/*
 * Initialize the timer queue.
 *
 * Requires initialized kernel timekeeping.
 */
timer_queue_status_t timer_queue_init(void);

bool timer_queue_is_initialized(void);


/*
 * Schedule a one-shot timer.
 *
 * delay_ns:
 *     Time from now until expiration, in nanoseconds.
 *
 * callback:
 *     Function called later in normal kernel context.
 *
 * context:
 *     User-provided callback argument.
 *
 * handle_out:
 *     Receives the timer handle.
 */
timer_queue_status_t timer_queue_schedule_after(
    uint64_t delay_ns,
    timer_callback_t callback,
    void *context,
    timer_handle_t *handle_out
);


/*
 * Schedule a periodic timer.
 *
 * interval_ns must be nonzero.
 *
 * The timer is first triggered after interval_ns.
 */
timer_queue_status_t timer_queue_schedule_periodic(
    uint64_t interval_ns,
    timer_callback_t callback,
    void *context,
    timer_handle_t *handle_out
);


/*
 * Cancel an active timer.
 *
 * A callback already placed in the deferred queue
 * is not withdrawn by cancellation.
 */
timer_queue_status_t timer_queue_cancel(
    timer_handle_t handle
);


/*
 * Check for expired timers.
 *
 * Called by the LAPIC timer interrupt handler.
 *
 * This function must not execute callbacks.
 */
timer_queue_status_t timer_queue_poll(void);


/*
 * Execute callbacks in normal kernel context.
 *
 * max_callbacks:
 *     Maximum callbacks to execute.
 *     Zero means drain all currently queued callbacks.
 *
 * Returns the number of callbacks executed.
 */
size_t timer_queue_run_deferred(size_t max_callbacks);


/*
 * Retrieve timer queue statistics.
 */
bool timer_queue_get_stats(
    timer_queue_stats_t *stats
);

#endif
