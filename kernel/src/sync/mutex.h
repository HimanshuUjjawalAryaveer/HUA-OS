#ifndef HUA_MUTEX_H
#define HUA_MUTEX_H

#include <stdbool.h>

#include "scheduler/thread.h"
#include "scheduler/wait_queue.h"

/*
 * Sleeping mutex.
 *
 * Ownership is associated with the thread that acquired it.
 *
 * Current implementation:
 * - CPU0 only.
 * - Ordinary thread context only.
 * - FIFO waiter ordering.
 * - Direct ownership handoff.
 */
typedef struct mutex {

    struct thread *owner;

    struct wait_queue waiters;

    bool initialized;

} mutex_t;


/*
 * Initialize a mutex.
 *
 * The mutex storage must be zero-initialized before
 * its first initialization.
 */
bool mutex_init(mutex_t *mutex);


/*
 * Acquire a mutex.
 *
 * If another thread owns it, the caller blocks until
 * ownership is handed to it.
 *
 * Recursive acquisition is rejected.
 */
bool mutex_lock(mutex_t *mutex);


/*
 * Attempt to acquire a mutex without blocking.
 *
 * Returns false if another thread owns it.
 */
bool mutex_try_lock(mutex_t *mutex);


/*
 * Release a mutex.
 *
 * Only the owning thread may unlock it.
 */
bool mutex_unlock(mutex_t *mutex);


/*
 * Return whether the mutex currently has an owner.
 */
bool mutex_is_locked(const mutex_t *mutex);

#endif
