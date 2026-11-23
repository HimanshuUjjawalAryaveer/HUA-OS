
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "timer_queue.h"
#include "time.h"


/* ============================================================
 * Internal data structures
 * ============================================================ */

/*
 * One timer slot.
 *
 * Timers are stored in a fixed-size array. The binary min-heap
 * stores slot indices rather than copying timer structures.
 */
struct timer_node {

    bool active;

    uint32_t generation;

    uint64_t deadline_ns;
    uint64_t period_ns;

    timer_callback_t callback;
    void *context;

    uint16_t heap_index;
};


/*
 * A callback record is independent of the timer slot.
 *
 * This is important because a one-shot timer's slot can be
 * released immediately after expiration while its callback
 * is still waiting to execute.
 */
struct timer_callback_record {

    timer_callback_t callback;
    void *context;
};


/*
 * Complete timer queue state.
 */
struct timer_queue_state {

    bool initialized;

    struct timer_node timers[TIMER_QUEUE_MAX_TIMERS];

    /*
     * Binary min-heap containing timer slot indices.
     *
     * The timer with the earliest deadline is always at index 0.
     */
    uint16_t heap[TIMER_QUEUE_MAX_TIMERS];

    size_t heap_count;

    /*
     * Deferred callback ring buffer.
     */
    struct timer_callback_record callbacks[
        TIMER_QUEUE_MAX_CALLBACKS
    ];

    size_t callback_head;
    size_t callback_tail;
    size_t callback_count;

    timer_queue_stats_t stats;
};


static struct timer_queue_state timer_queue;


/* ============================================================
 * Interrupt protection
 * ============================================================ */

/*
 * HUA currently uses a single CPU.
 *
 * Disabling local interrupts protects shared timer queue
 * structures against the LAPIC timer interrupt handler.
 *
 * This is not an SMP lock. When HUA gains multiple CPUs,
 * this protection must be replaced or supplemented with
 * an appropriate spinlock.
 */

static inline uint64_t timer_queue_irq_save(void) {

    uint64_t flags;

    __asm__ volatile (
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}


static inline void timer_queue_irq_restore(uint64_t flags) {

    /*
     * Restore the interrupt-enable state that existed
     * before entering the critical section.
     */
    if (flags & (1ULL << 9)) {

        __asm__ volatile (
            "sti"
            :
            :
            : "memory"
        );
    }
}


/* ============================================================
 * Timer generation management
 * ============================================================ */

static uint32_t timer_queue_next_generation(
    uint32_t generation
) {

    generation++;

    /*
     * Generation zero is reserved as invalid.
     */
    if (generation == 0) {
        generation = 1;
    }

    return generation;
}


/* ============================================================
 * Binary min-heap
 * ============================================================ */

static bool timer_queue_timer_less(
    uint16_t left_slot,
    uint16_t right_slot
) {

    const struct timer_node *left =
        &timer_queue.timers[left_slot];

    const struct timer_node *right =
        &timer_queue.timers[right_slot];

    if (left->deadline_ns != right->deadline_ns) {

        return left->deadline_ns <
               right->deadline_ns;
    }

    /*
     * Deterministic ordering for equal deadlines.
     */
    return left_slot < right_slot;
}


static void timer_queue_heap_swap(
    size_t left,
    size_t right
) {

    uint16_t left_slot = timer_queue.heap[left];
    uint16_t right_slot = timer_queue.heap[right];

    timer_queue.heap[left] = right_slot;
    timer_queue.heap[right] = left_slot;

    timer_queue.timers[left_slot].heap_index =
        (uint16_t)right;

    timer_queue.timers[right_slot].heap_index =
        (uint16_t)left;
}


static void timer_queue_heap_sift_up(
    size_t index
) {

    while (index > 0) {

        size_t parent = (index - 1) / 2;

        if (!timer_queue_timer_less(
                timer_queue.heap[index],
                timer_queue.heap[parent])) {

            break;
        }

        timer_queue_heap_swap(index, parent);

        index = parent;
    }
}


static void timer_queue_heap_sift_down(
    size_t index
) {

    while (true) {

        size_t left = index * 2 + 1;
        size_t right = index * 2 + 2;

        size_t smallest = index;

        if (left < timer_queue.heap_count &&
            timer_queue_timer_less(
                timer_queue.heap[left],
                timer_queue.heap[smallest])) {

            smallest = left;
        }

        if (right < timer_queue.heap_count &&
            timer_queue_timer_less(
                timer_queue.heap[right],
                timer_queue.heap[smallest])) {

            smallest = right;
        }

        if (smallest == index) {
            break;
        }

        timer_queue_heap_swap(index, smallest);

        index = smallest;
    }
}


static bool timer_queue_heap_insert(
    uint16_t slot
) {

    if (timer_queue.heap_count >=
        TIMER_QUEUE_MAX_TIMERS) {

        return false;
    }

    size_t index = timer_queue.heap_count;

    timer_queue.heap[index] = slot;

    timer_queue.timers[slot].heap_index =
        (uint16_t)index;

    timer_queue.heap_count++;

    timer_queue_heap_sift_up(index);

    return true;
}


static uint16_t timer_queue_heap_remove_at(
    size_t index
) {

    uint16_t removed_slot = timer_queue.heap[index];

    size_t last_index = timer_queue.heap_count - 1;

    timer_queue.heap_count--;

    if (index != last_index) {

        uint16_t replacement =
            timer_queue.heap[last_index];

        timer_queue.heap[index] = replacement;

        timer_queue.timers[replacement].heap_index =
            (uint16_t)index;

        if (index > 0 &&
            timer_queue_timer_less(
                replacement,
                timer_queue.heap[(index - 1) / 2])) {

            timer_queue_heap_sift_up(index);

        } else {

            timer_queue_heap_sift_down(index);
        }
    }

    timer_queue.timers[removed_slot].heap_index =
        TIMER_QUEUE_INVALID_SLOT;

    return removed_slot;
}


static void timer_queue_heap_remove_timer(
    uint16_t slot
) {

    size_t index =
        timer_queue.timers[slot].heap_index;

    if (index >= timer_queue.heap_count) {
        return;
    }

    timer_queue_heap_remove_at(index);
}


static void timer_queue_heap_reorder_timer(
    uint16_t slot
) {

    size_t index =
        timer_queue.timers[slot].heap_index;

    if (index >= timer_queue.heap_count) {
        return;
    }

    if (index > 0 &&
        timer_queue_timer_less(
            slot,
            timer_queue.heap[(index - 1) / 2])) {

        timer_queue_heap_sift_up(index);

    } else {

        timer_queue_heap_sift_down(index);
    }
}


/* ============================================================
 * Timer slot management
 * ============================================================ */

static uint16_t timer_queue_find_free_slot(void) {

    for (uint16_t i = 0;
         i < TIMER_QUEUE_MAX_TIMERS;
         i++) {

        if (!timer_queue.timers[i].active) {
            return i;
        }
    }

    return TIMER_QUEUE_INVALID_SLOT;
}


static void timer_queue_release_slot(
    uint16_t slot
) {

    struct timer_node *timer =
        &timer_queue.timers[slot];

    timer->active = false;

    timer->deadline_ns = 0;
    timer->period_ns = 0;

    timer->callback = NULL;
    timer->context = NULL;

    timer->heap_index =
        TIMER_QUEUE_INVALID_SLOT;

    timer->generation =
        timer_queue_next_generation(
            timer->generation
        );

    if (timer_queue.stats.active_timers > 0) {
        timer_queue.stats.active_timers--;
    }
}


/* ============================================================
 * Handle validation
 * ============================================================ */

static bool timer_queue_handle_is_valid(
    timer_handle_t handle
) {

    if (handle.slot >= TIMER_QUEUE_MAX_TIMERS) {
        return false;
    }

    if (handle.generation == 0) {
        return false;
    }

    const struct timer_node *timer =
        &timer_queue.timers[handle.slot];

    return timer->active &&
           timer->generation == handle.generation;
}


/* ============================================================
 * Deferred callback ring buffer
 * ============================================================ */

static bool timer_queue_callback_enqueue(
    timer_callback_t callback,
    void *context
) {

    if (timer_queue.callback_count >=
        TIMER_QUEUE_MAX_CALLBACKS) {

        return false;
    }

    struct timer_callback_record *record =
        &timer_queue.callbacks[
            timer_queue.callback_tail
        ];

    record->callback = callback;
    record->context = context;

    timer_queue.callback_tail++;

    if (timer_queue.callback_tail >=
        TIMER_QUEUE_MAX_CALLBACKS) {

        timer_queue.callback_tail = 0;
    }

    timer_queue.callback_count++;

    timer_queue.stats.callbacks_queued++;

    return true;
}


static bool timer_queue_callback_dequeue(
    struct timer_callback_record *record
) {

    if (record == NULL ||
        timer_queue.callback_count == 0) {

        return false;
    }

    *record =
        timer_queue.callbacks[
            timer_queue.callback_head
        ];

    timer_queue.callback_head++;

    if (timer_queue.callback_head >=
        TIMER_QUEUE_MAX_CALLBACKS) {

        timer_queue.callback_head = 0;
    }

    timer_queue.callback_count--;

    return true;
}


/* ============================================================
 * Timer scheduling
 * ============================================================ */

static timer_queue_status_t timer_queue_schedule_internal(
    uint64_t delay_ns,
    uint64_t period_ns,
    timer_callback_t callback,
    void *context,
    timer_handle_t *handle_out
) {

    if (!timer_queue.initialized) {
        return TIMER_QUEUE_STATUS_NOT_INITIALIZED;
    }

    if (delay_ns == 0 ||
        callback == NULL ||
        handle_out == NULL) {

        return TIMER_QUEUE_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Obtain current monotonic time before entering
     * the critical section.
     */
    uint64_t now_ns = 0;

    if (time_get_monotonic_ns(&now_ns) !=
        TIME_STATUS_OK) {

        return TIMER_QUEUE_STATUS_TIME_FAILURE;
    }

    /*
     * Prevent deadline arithmetic overflow.
     */
    if (delay_ns > UINT64_MAX - now_ns) {
        return TIMER_QUEUE_STATUS_TIME_OVERFLOW;
    }

    uint64_t deadline_ns = now_ns + delay_ns;

    uint64_t flags = timer_queue_irq_save();

    uint16_t slot =
        timer_queue_find_free_slot();

    if (slot == TIMER_QUEUE_INVALID_SLOT) {

        timer_queue_irq_restore(flags);

        return TIMER_QUEUE_STATUS_NO_TIMER_SLOTS;
    }

    struct timer_node *timer =
        &timer_queue.timers[slot];

    timer->active = true;

    /*
     * The generation was initialized to a nonzero value
     * and changes every time the slot is released.
     */
    if (timer->generation == 0) {
        timer->generation = 1;
    }

    timer->deadline_ns = deadline_ns;
    timer->period_ns = period_ns;

    timer->callback = callback;
    timer->context = context;

    timer->heap_index =
        TIMER_QUEUE_INVALID_SLOT;

    if (!timer_queue_heap_insert(slot)) {

        timer->active = false;
        timer->callback = NULL;
        timer->context = NULL;

        timer_queue_irq_restore(flags);

        return TIMER_QUEUE_STATUS_INTERNAL_ERROR;
    }

    timer_queue.stats.active_timers++;

    if (timer_queue.stats.active_timers >
        timer_queue.stats.peak_active_timers) {

        timer_queue.stats.peak_active_timers =
            timer_queue.stats.active_timers;
    }

    timer_queue.stats.timers_created++;

    handle_out->slot = slot;
    handle_out->generation = timer->generation;

    timer_queue_irq_restore(flags);

    return TIMER_QUEUE_STATUS_OK;
}


/* ============================================================
 * Public initialization API
 * ============================================================ */

timer_queue_status_t timer_queue_init(void) {

    if (timer_queue.initialized) {
        return TIMER_QUEUE_STATUS_ALREADY_INITIALIZED;
    }

    if (!time_is_initialized()) {
        return TIMER_QUEUE_STATUS_TIME_FAILURE;
    }

    /*
     * Clear all queue state.
     */
    timer_queue = (struct timer_queue_state){0};

    for (size_t i = 0;
         i < TIMER_QUEUE_MAX_TIMERS;
         i++) {

        timer_queue.timers[i].generation = 1;

        timer_queue.timers[i].heap_index =
            TIMER_QUEUE_INVALID_SLOT;
    }

    timer_queue.stats.initialized = true;

    timer_queue.initialized = true;

    return TIMER_QUEUE_STATUS_OK;
}


bool timer_queue_is_initialized(void) {

    return timer_queue.initialized;
}


/* ============================================================
 * Public scheduling API
 * ============================================================ */

timer_queue_status_t timer_queue_schedule_after(
    uint64_t delay_ns,
    timer_callback_t callback,
    void *context,
    timer_handle_t *handle_out
) {

    return timer_queue_schedule_internal(
        delay_ns,
        0,
        callback,
        context,
        handle_out
    );
}


timer_queue_status_t timer_queue_schedule_periodic(
    uint64_t interval_ns,
    timer_callback_t callback,
    void *context,
    timer_handle_t *handle_out
) {

    if (interval_ns == 0) {
        return TIMER_QUEUE_STATUS_INVALID_ARGUMENT;
    }

    return timer_queue_schedule_internal(
        interval_ns,
        interval_ns,
        callback,
        context,
        handle_out
    );
}


/* ============================================================
 * Public cancellation API
 * ============================================================ */

timer_queue_status_t timer_queue_cancel(
    timer_handle_t handle
) {

    if (!timer_queue.initialized) {
        return TIMER_QUEUE_STATUS_NOT_INITIALIZED;
    }

    uint64_t flags = timer_queue_irq_save();

    if (!timer_queue_handle_is_valid(handle)) {

        timer_queue_irq_restore(flags);

        return TIMER_QUEUE_STATUS_INVALID_HANDLE;
    }

    timer_queue_heap_remove_timer(handle.slot);

    timer_queue_release_slot(handle.slot);

    timer_queue.stats.timers_cancelled++;

    timer_queue_irq_restore(flags);

    return TIMER_QUEUE_STATUS_OK;
}


/* ============================================================
 * Expiration processing
 * ============================================================ */

/*
 * Called from the LAPIC timer interrupt handler.
 *
 * This function:
 *
 * 1. Reads monotonic time.
 * 2. Finds expired timers.
 * 3. Places callback records in the deferred queue.
 * 4. Removes expired one-shot timers.
 * 5. Advances periodic timer deadlines.
 *
 * It NEVER executes callbacks.
 */

timer_queue_status_t timer_queue_poll(void) {

    if (!timer_queue.initialized) {
        return TIMER_QUEUE_STATUS_NOT_INITIALIZED;
    }

    uint64_t now_ns = 0;

    if (time_get_monotonic_ns(&now_ns) !=
        TIME_STATUS_OK) {

        return TIMER_QUEUE_STATUS_TIME_FAILURE;
    }

    uint64_t flags = timer_queue_irq_save();

    timer_queue.stats.poll_count++;

    while (timer_queue.heap_count > 0) {

        uint16_t slot = timer_queue.heap[0];

        struct timer_node *timer =
            &timer_queue.timers[slot];

        /*
         * The earliest timer has not expired.
         * No other timer can have expired either.
         */
        if (timer->deadline_ns > now_ns) {
            break;
        }

        /*
         * Never discard an expiration if the callback queue
         * is full. Leave this timer at the heap root and
         * retry during a later interrupt.
         */
        if (timer_queue.callback_count >=
            TIMER_QUEUE_MAX_CALLBACKS) {

            timer_queue.stats.callback_queue_overflows++;

            timer_queue_irq_restore(flags);

            return TIMER_QUEUE_STATUS_CALLBACK_QUEUE_FULL;
        }

        /*
         * Queue the callback without executing it.
         */
        if (!timer_queue_callback_enqueue(
                timer->callback,
                timer->context)) {

            timer_queue.stats.callback_queue_overflows++;

            timer_queue_irq_restore(flags);

            return TIMER_QUEUE_STATUS_CALLBACK_QUEUE_FULL;
        }

        timer_queue.stats.timers_expired++;

        /*
         * One-shot timer.
         */
        if (timer->period_ns == 0) {

            timer_queue_heap_remove_at(0);

            timer_queue_release_slot(slot);

            continue;
        }

        /*
         * Periodic timer.
         *
         * Advance from the previous deadline, not from
         * the current time. This avoids cumulative drift.
         *
         * If multiple periods were missed, skip them and
         * schedule the next future deadline.
         */
        timer_queue.stats.periodic_expirations++;

        uint64_t elapsed_ns =
            now_ns - timer->deadline_ns;

        uint64_t missed_periods =
            elapsed_ns / timer->period_ns;

        /*
         * Include the next interval after the current
         * expiration.
         */
        uint64_t intervals = missed_periods + 1;

        /*
         * Check multiplication and addition overflow.
         *
         * If the next periodic deadline cannot be
         * represented, deliver this callback and retire
         * the timer safely.
         */
        bool deadline_overflow = false;

        if (intervals >
            (UINT64_MAX - timer->deadline_ns) /
            timer->period_ns) {

            deadline_overflow = true;
        }

        if (deadline_overflow) {

            timer_queue_heap_remove_at(0);

            timer_queue_release_slot(slot);

            continue;
        }

        timer->deadline_ns +=
            intervals * timer->period_ns;

        /*
         * Its deadline moved forward, so restore
         * the min-heap ordering.
         */
        timer_queue_heap_sift_down(0);
    }

    timer_queue_irq_restore(flags);

    return TIMER_QUEUE_STATUS_OK;
}


/* ============================================================
 * Deferred callback execution
 * ============================================================ */

/*
 * Execute callbacks in ordinary kernel context.
 *
 * The queue lock is never held while calling a callback.
 * Therefore callbacks may safely schedule or cancel timers.
 *
 * max_callbacks == 0 means execute the callbacks that
 * were pending when this function was entered.
 */

size_t timer_queue_run_deferred(
    size_t max_callbacks
) {

    if (!timer_queue.initialized) {
        return 0;
    }

    size_t execution_limit = max_callbacks;

    if (execution_limit == 0) {

        uint64_t flags = timer_queue_irq_save();

        execution_limit = timer_queue.callback_count;

        timer_queue_irq_restore(flags);
    }

    size_t executed = 0;

    while (executed < execution_limit) {

        struct timer_callback_record record;

        uint64_t flags = timer_queue_irq_save();

        bool available =
            timer_queue_callback_dequeue(&record);

        timer_queue_irq_restore(flags);

        if (!available) {
            break;
        }

        /*
         * Callbacks execute with the caller's original
         * interrupt state restored.
         */
        if (record.callback != NULL) {

            record.callback(record.context);
        }

        flags = timer_queue_irq_save();

        timer_queue.stats.callbacks_executed++;

        timer_queue_irq_restore(flags);

        executed++;
    }

    return executed;
}


/* ============================================================
 * Statistics
 * ============================================================ */

bool timer_queue_get_stats(
    timer_queue_stats_t *stats
) {

    if (stats == NULL ||
        !timer_queue.initialized) {

        return false;
    }

    uint64_t flags = timer_queue_irq_save();

    *stats = timer_queue.stats;

    /*
     * The public initialized field reflects the
     * actual queue state.
     */
    stats->initialized = timer_queue.initialized;

    timer_queue_irq_restore(flags);

    return true;
}
