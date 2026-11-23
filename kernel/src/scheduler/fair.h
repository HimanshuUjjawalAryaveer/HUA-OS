#ifndef HUA_SCHEDULER_FAIR_H
#define HUA_SCHEDULER_FAIR_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler/scheduler.h"

/*
 * Bound total runnable fair weight.
 *
 * This limit keeps intermediate weighted-average
 * multiplication within uint64_t.
 */
#define SCHEDULER_FAIR_MAX_RUNQUEUE_WEIGHT (1ULL << 40)

/*
 * Change the weight of a NEW fair thread.
 *
 * Weight changes are restricted to NEW threads for now.
 * A future scheduler control API will handle changes
 * to runnable threads safely.
 */
bool scheduler_fair_set_weight(
    struct thread *thread,
    uint64_t weight
);

/*
 * Account elapsed execution time for a fair thread.
 *
 * elapsed_ns:
 *     Actual execution time in nanoseconds.
 *
 * Updates:
 *     runtime_ns
 *     virtual_runtime
 *
 * Does not perform queue operations.
 */
bool scheduler_fair_account_runtime(
    struct thread *thread,
    uint64_t elapsed_ns
);

/*
 * Calculate the virtual deadline of the thread's
 * current scheduling request.
 */
bool scheduler_fair_update_deadline(
    struct thread *thread
);

/*
 * Recalculate weighted average and minimum virtual
 * runtime for the fair run queue.
 */
void scheduler_fair_recalculate(
    struct scheduler_run_queue *queue
);

/*
 * Select the eligible fair thread with the earliest
 * virtual deadline.
 *
 * Does not remove the selected thread.
 */
struct thread *scheduler_fair_pick(
    struct scheduler_run_queue *queue
);

/*
 * Check whether a fair thread has consumed its scheduling
 * request.
 */
bool scheduler_fair_slice_expired(
    const struct thread *thread
);

/*
 * Check whether an expired fair thread should trigger
 * a fair-to-fair scheduling decision.
 *
 * Returns true only when:
 * - The current thread is fair.
 * - Its fair slice has expired.
 * - Another fair thread is runnable.
 */
bool scheduler_fair_should_preempt(
    const struct thread *thread,
    const struct scheduler_run_queue *queue
);

#endif
