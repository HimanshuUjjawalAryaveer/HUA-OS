#ifndef RWLOCK_H
#define RWLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler/wait_queue.h"

struct thread;
struct rwlock_reader;

typedef struct rwlock {
    uint64_t active_readers;
    uint64_t waiting_writers;

    struct thread *writer;

    struct rwlock_reader *reader_owners;

    struct wait_queue reader_waiters;
    struct wait_queue writer_waiters;

    bool initialized;
} rwlock_t;

/*
 * RW-lock usage rules:
 *
 * - CPU0 only; this implementation is not SMP-safe.
 * - Blocking operations require ordinary thread context.
 * - Do not use blocking operations from interrupt handlers.
 * - Recursive read acquisition is rejected.
 * - Recursive write acquisition is rejected.
 * - Read-to-write upgrades are rejected.
 * - A thread must release only the lock it owns.
 * - Do not block, sleep, or yield while holding a lock
 *   unless the specific operation is part of a deliberate
 *   synchronization protocol.
 *
 * Writer preference is used: new readers cannot bypass
 * writers that are already waiting.
 */

/*
 * Initialize a zero-initialized RW lock.
 *
 * The object must be zero-initialized before its first
 * initialization, for example:
 *
 *     rwlock_t lock = {0};
 *
 * Reinitializing an already initialized lock is rejected.
 */
bool rwlock_init(rwlock_t *lock);

bool rwlock_read_lock(rwlock_t *lock);
bool rwlock_try_read_lock(rwlock_t *lock);
bool rwlock_read_unlock(rwlock_t *lock);

bool rwlock_write_lock(rwlock_t *lock);
bool rwlock_try_write_lock(rwlock_t *lock);
bool rwlock_write_unlock(rwlock_t *lock);

bool rwlock_is_initialized(const rwlock_t *lock);
uint64_t rwlock_reader_count(const rwlock_t *lock);
bool rwlock_writer_is_active(const rwlock_t *lock);

#endif
