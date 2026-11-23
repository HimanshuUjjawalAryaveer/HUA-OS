#ifndef HUA_SEMAPHORE_H
#define HUA_SEMAPHORE_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler/wait_queue.h"

/*
 * Counting semaphore.
 *
 * The semaphore tracks available permits.
 *
 * Waiting threads are stored in a FIFO wait queue.
 *
 * The semaphore does not have an owner.
 *
 * Current implementation:
 * - CPU0 only.
 * - Interrupt-safe internal state protection.
 * - Blocking operations require ordinary thread context.
 * - Not yet SMP-safe.
 */
typedef struct semaphore {

    uint64_t count;

    struct wait_queue waiters;

    bool initialized;

} semaphore_t;


/*
 * Initialize a semaphore.
 *
 * initial_count specifies the number of
 * initially available permits.
 *
 * Storage must be zero-initialized before
 * its first initialization.
 */
bool semaphore_init(
    semaphore_t *semaphore,
    uint64_t initial_count
);


/*
 * Acquire one permit.
 *
 * If no permit is available, the current
 * thread blocks until another thread releases
 * a permit.
 *
 * Must be called from ordinary thread context.
 */
bool semaphore_down(
    semaphore_t *semaphore
);


/*
 * Attempt to acquire one permit without blocking.
 */
bool semaphore_try_down(
    semaphore_t *semaphore
);


/*
 * Release one permit.
 *
 * If threads are waiting, one waiter receives
 * the permit directly.
 */
bool semaphore_up(
    semaphore_t *semaphore
);


/*
 * Return the currently available permit count.
 *
 * Returns zero for an invalid or uninitialized
 * semaphore.
 */
uint64_t semaphore_get_count(
    const semaphore_t *semaphore
);


/*
 * Check whether the semaphore is initialized.
 */
bool semaphore_is_initialized(
    const semaphore_t *semaphore
);

#endif
