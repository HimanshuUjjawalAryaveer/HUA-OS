#include "sync/rwlock.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/irq_flags.h"
#include "mm/heap.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "scheduler/wait_queue.h"


/*
 * One record represents one thread holding a read lock.
 *
 * Records are private to this implementation.
 */
struct rwlock_reader {

    struct thread *thread;

    struct rwlock_reader *next;
};


/*
 * Find the reader ownership record belonging to a thread.
 *
 * Caller must have interrupts disabled.
 */
static struct rwlock_reader *rwlock_find_reader_locked(
    rwlock_t *lock,
    struct thread *thread
) {

    struct rwlock_reader *reader =
        lock->reader_owners;

    while (reader != NULL) {

        if (reader->thread == thread) {
            return reader;
        }

        reader = reader->next;
    }

    return NULL;
}


/*
 * Validate an ordinary current thread.
 *
 * Caller must have interrupts disabled.
 */
static struct thread *rwlock_get_current_thread_locked(void) {

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING ||
        current->scheduling_class == THREAD_CLASS_IDLE ||
        current->preempt_count != 0) {

        return NULL;
    }

    return current;
}


/*
 * Initialize an RW lock.
 */
bool rwlock_init(rwlock_t *lock) {

    if (lock == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (lock->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    lock->active_readers = 0;
    lock->waiting_writers = 0;

    lock->writer = NULL;
    lock->reader_owners = NULL;

    if (!wait_queue_init(&lock->reader_waiters) ||
        !wait_queue_init(&lock->writer_waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    lock->initialized = true;

    arch_irq_restore(flags);

    return true;
}


/*
 * Acquire a read lock, blocking when necessary.
 *
 * Reader ownership memory is allocated before entering
 * the IRQ-disabled critical section.
 */
bool rwlock_read_lock(rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return false;
    }

    struct rwlock_reader *record =
        kmalloc(sizeof(struct rwlock_reader));

    if (record == NULL) {
        return false;
    }

    record->thread = NULL;
    record->next = NULL;

    uint64_t flags = arch_irq_save_disable();

    struct thread *current =
        rwlock_get_current_thread_locked();

    if (current == NULL ||
        !lock->initialized ||
        rwlock_find_reader_locked(lock, current) != NULL ||
        lock->writer == current) {

        arch_irq_restore(flags);
        kfree(record);
        return false;
    }

    for (;;) {

        /*
         * Writer preference:
         *
         * Readers may enter only when no writer owns
         * the lock and no writer is waiting.
         */
        if (lock->writer == NULL &&
            lock->waiting_writers == 0) {

            if (lock->active_readers == UINT64_MAX) {

                arch_irq_restore(flags);
                kfree(record);
                return false;
            }

            record->thread = current;
            record->next = lock->reader_owners;

            lock->reader_owners = record;
            lock->active_readers++;

            arch_irq_restore(flags);

            return true;
        }

        /*
         * A writer is active or waiting.
         * Sleep until readers are allowed to proceed.
         *
         * Interrupts remain disabled across the check
         * and thread_block(), preventing lost wakeups.
         */
        if (!wait_queue_validate(&lock->reader_waiters)) {

            arch_irq_restore(flags);
            kfree(record);
            return false;
        }

        if (!thread_block(&lock->reader_waiters)) {

            arch_irq_restore(flags);
            kfree(record);
            return false;
        }

        /*
         * We resumed.
         *
         * thread_block() restores the interrupt state
         * that existed when it was called: disabled.
         *
         * Retry the acquisition conditions.
         */
    }
}


/*
 * Attempt to acquire a read lock without blocking.
 */
bool rwlock_try_read_lock(rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return false;
    }

    struct rwlock_reader *record =
        kmalloc(sizeof(struct rwlock_reader));

    if (record == NULL) {
        return false;
    }

    record->thread = NULL;
    record->next = NULL;

    uint64_t flags = arch_irq_save_disable();

    struct thread *current =
        rwlock_get_current_thread_locked();

    if (current == NULL ||
        !lock->initialized ||
        lock->writer != NULL ||
        lock->waiting_writers != 0 ||
        lock->active_readers == UINT64_MAX ||
        rwlock_find_reader_locked(lock, current) != NULL) {

        arch_irq_restore(flags);
        kfree(record);
        return false;
    }

    record->thread = current;
    record->next = lock->reader_owners;

    lock->reader_owners = record;
    lock->active_readers++;

    arch_irq_restore(flags);

    return true;
}


/*
 * Release a read lock.
 */
bool rwlock_read_unlock(rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    struct thread *current =
        rwlock_get_current_thread_locked();

    if (current == NULL) {

        arch_irq_restore(flags);
        return false;
    }

    struct rwlock_reader **link =
        &lock->reader_owners;

    while (*link != NULL &&
           (*link)->thread != current) {

        link = &(*link)->next;
    }

    struct rwlock_reader *record = *link;

    if (record == NULL ||
        lock->active_readers == 0) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Remove this reader's ownership record.
     */
    *link = record->next;
    lock->active_readers--;

    /*
     * If this was the last reader, a waiting writer
     * gets priority.
     */
    if (lock->active_readers == 0 &&
        lock->waiting_writers != 0) {

        struct thread *next_writer =
            lock->writer_waiters.head;

        if (next_writer == NULL) {

            /*
             * Inconsistent waiting-writer state.
             * Restore the reader ownership.
             */
            record->next = lock->reader_owners;
            lock->reader_owners = record;
            lock->active_readers++;

            arch_irq_restore(flags);
            return false;
        }

        /*
         * Transfer ownership before waking the writer.
         */
        lock->writer = next_writer;

        if (!wait_queue_wake_one(&lock->writer_waiters)) {

            lock->writer = NULL;

            record->next = lock->reader_owners;
            lock->reader_owners = record;
            lock->active_readers++;

            arch_irq_restore(flags);
            return false;
        }
    }

    arch_irq_restore(flags);

    /*
     * Heap operations happen outside the critical section.
     */
    kfree(record);

    return true;
}


/*
 * Acquire a write lock, blocking when necessary.
 */
bool rwlock_write_lock(rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    struct thread *current =
        rwlock_get_current_thread_locked();

    if (current == NULL ||
        !lock->initialized ||
        lock->writer == current ||
        rwlock_find_reader_locked(lock, current) != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    for (;;) {

        /*
         * A writer may acquire only when:
         * - there is no active writer;
         * - there are no active readers;
         * - there are no earlier waiting writers.
         */
        if (lock->writer == NULL &&
            lock->active_readers == 0 &&
            lock->waiting_writers == 0) {

            lock->writer = current;

            arch_irq_restore(flags);
            return true;
        }

        if (lock->waiting_writers == UINT64_MAX) {

            arch_irq_restore(flags);
            return false;
        }

        if (!wait_queue_validate(&lock->writer_waiters)) {

            arch_irq_restore(flags);
            return false;
        }

        /*
         * Count this writer before blocking.
         * The IRQ-disabled region prevents another
         * thread from observing a partial transition.
         */
        lock->waiting_writers++;

        if (!thread_block(&lock->writer_waiters)) {

            lock->waiting_writers--;

            arch_irq_restore(flags);
            return false;
        }

        /*
         * We resumed after being selected by the scheduler.
         *
         * A releasing thread may already have transferred
         * writer ownership directly to us.
         */
        if (lock->waiting_writers == 0) {

            arch_irq_restore(flags);
            return false;
        }

        lock->waiting_writers--;

        if (lock->writer == current) {

            arch_irq_restore(flags);
            return true;
        }

        /*
         * Defensive retry if we woke without receiving
         * ownership.
         */
    }
}


/*
 * Attempt to acquire a write lock without blocking.
 */
bool rwlock_try_write_lock(rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    struct thread *current =
        rwlock_get_current_thread_locked();

    if (current == NULL ||
        !lock->initialized ||
        lock->writer != NULL ||
        lock->active_readers != 0 ||
        lock->waiting_writers != 0 ||
        rwlock_find_reader_locked(lock, current) != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    lock->writer = current;

    arch_irq_restore(flags);

    return true;
}


/*
 * Release a write lock.
 */
bool rwlock_write_unlock(rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    struct thread *current =
        rwlock_get_current_thread_locked();

    if (current == NULL ||
        lock->writer != current) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Writers have priority over readers.
     */
    if (!wait_queue_is_empty(&lock->writer_waiters)) {

        struct thread *next_writer =
            lock->writer_waiters.head;

        if (next_writer == NULL) {

            arch_irq_restore(flags);
            return false;
        }

        /*
         * Direct ownership handoff.
         */
        lock->writer = next_writer;

        if (!wait_queue_wake_one(&lock->writer_waiters)) {

            lock->writer = current;

            arch_irq_restore(flags);
            return false;
        }

        arch_irq_restore(flags);

        return true;
    }

    /*
     * No waiting writers.
     * Readers may now proceed together.
     */
    if (lock->waiting_writers != 0) {

        /*
         * This should not occur: waiting_writers must
         * correspond to queued writers at this point.
         */
        arch_irq_restore(flags);
        return false;
    }

    uint64_t reader_waiter_count =
        wait_queue_count(&lock->reader_waiters);

    lock->writer = NULL;

    uint64_t awakened =
        wait_queue_wake_all(&lock->reader_waiters);

    /*
     * If wakeup was incomplete, restore writer ownership.
     * Any readers already awakened will recheck the lock
     * and block again if they run after this restoration.
     */
    if (awakened != reader_waiter_count) {

        lock->writer = current;

        arch_irq_restore(flags);
        return false;
    }

    arch_irq_restore(flags);

    return true;
}


/*
 * Query whether the lock is initialized.
 */
bool rwlock_is_initialized(const rwlock_t *lock) {

    return lock != NULL && lock->initialized;
}


/*
 * Return the number of active readers.
 */
uint64_t rwlock_reader_count(const rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return 0;
    }

    uint64_t flags = arch_irq_save_disable();

    uint64_t count =
        lock->active_readers;

    arch_irq_restore(flags);

    return count;
}


/*
 * Return whether a writer currently owns the lock.
 */
bool rwlock_writer_is_active(const rwlock_t *lock) {

    if (lock == NULL || !lock->initialized) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool active =
        lock->writer != NULL;

    arch_irq_restore(flags);

    return active;
}
