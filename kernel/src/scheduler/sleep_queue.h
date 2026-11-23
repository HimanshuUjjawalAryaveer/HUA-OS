#ifndef HUA_SLEEP_QUEUE_H
#define HUA_SLEEP_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler/thread.h"

/*
 * Deadline-ordered intrusive sleep queue.
 *
 * Earliest deadline is always at the head.
 *
 * The queue does not own its threads.
 */
struct scheduler_sleep_queue {

    struct thread *head;
    struct thread *tail;

    uint64_t count;
};

/*
 * Initialize an empty sleep queue.
 */
void sleep_queue_init(
    struct scheduler_sleep_queue *queue
);

/*
 * Validate the complete queue structure.
 */
bool sleep_queue_validate(
    const struct scheduler_sleep_queue *queue
);

/*
 * Insert a SLEEPING thread in deadline order.
 */
bool sleep_queue_insert(
    struct scheduler_sleep_queue *queue,
    struct thread *thread
);

/*
 * Remove a particular sleeping thread.
 */
bool sleep_queue_remove(
    struct scheduler_sleep_queue *queue,
    struct thread *thread
);

/*
 * Remove the earliest sleeper if its deadline has passed.
 *
 * expired_deadline_ns receives the original deadline
 * so the scheduler can restore the sleeper if waking
 * it into the run queue fails.
 *
 * Returns NULL if:
 * - queue is NULL;
 * - expired_deadline_ns is NULL;
 * - the queue is invalid;
 * - no sleeper has expired.
 *
 * On success, the returned thread:
 * - is detached from the sleep queue;
 * - has all sleep metadata cleared;
 * - is in THREAD_STATE_READY.
 */
struct thread *sleep_queue_pop_expired(
    struct scheduler_sleep_queue *queue,
    uint64_t now_ns,
    uint64_t *expired_deadline_ns
);

/*
 * Restore a thread removed by sleep_queue_pop_expired()
 * when admitting it to the run queue fails.
 *
 * Restores the original deadline directly at the head.
 * This is O(1) and preserves FIFO order for equal deadlines.
 *
 * The thread must be detached and in THREAD_STATE_READY.
 * On success, this function changes it back to SLEEPING.
 */
bool sleep_queue_restore_expired(
    struct scheduler_sleep_queue *queue,
    struct thread *thread,
    uint64_t deadline_ns
);

/*
 * Return the number of queued sleepers.
 */
uint64_t sleep_queue_count(
    const struct scheduler_sleep_queue *queue
);

#endif
