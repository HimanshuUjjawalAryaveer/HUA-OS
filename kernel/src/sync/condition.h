#ifndef HUA_CONDITION_H
#define HUA_CONDITION_H

#include <stdbool.h>
#include <stdint.h>

#include "sync/mutex.h"
#include "scheduler/wait_queue.h"

/*
 * Condition variable.
 *
 * Threads sleep on its FIFO wait queue until signaled.
 *
 * A condition variable does not own a mutex.
 * The caller supplies the mutex used to protect
 * the condition being checked.
 *
 * Current implementation:
 * - CPU0 only.
 * - Ordinary thread context only.
 * - FIFO waiter ordering.
 * - No SMP support yet.
 */
typedef struct condition {

    struct wait_queue waiters;

    bool initialized;

} condition_t;


/*
 * Initialize a condition variable.
 *
 * Storage must be zero-initialized before its
 * first initialization.
 */
bool condition_init(condition_t *condition);


/*
 * Wait for a condition notification.
 *
 * The calling thread must own the supplied mutex.
 *
 * This function:
 * 1. Atomically releases the mutex and blocks.
 * 2. Waits until signaled.
 * 3. Reacquires the mutex before returning.
 *
 * The caller must check its actual condition again
 * after returning, normally using a while loop.
 */
bool condition_wait(
    condition_t *condition,
    mutex_t *mutex
);


/*
 * Wake one waiting thread.
 *
 * Returns true if the operation succeeds.
 * Returns true when there are no waiting threads.
 */
bool condition_signal(
    condition_t *condition
);


/*
 * Wake all currently waiting threads.
 *
 * Returns the number of threads successfully awakened.
 */
uint64_t condition_broadcast(
    condition_t *condition
);


/*
 * Return the number of waiting threads.
 */
uint64_t condition_waiter_count(
    const condition_t *condition
);


/*
 * Check whether the condition variable is initialized.
 */
bool condition_is_initialized(
    const condition_t *condition
);

#endif
