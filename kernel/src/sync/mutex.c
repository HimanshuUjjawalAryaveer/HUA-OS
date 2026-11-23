#include <stdbool.h>
#include <stdint.h>

#include "sync/mutex.h"

#include "arch/x86_64/irq_flags.h"
#include "scheduler/scheduler.h"


/* ============================================================
 * Initialization
 * ============================================================ */

bool mutex_init(mutex_t *mutex) {

    if (mutex == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * Reject repeated initialization.
     */
    if (mutex->initialized ||
        mutex->owner != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * The embedded wait queue must also be initialized
     * exactly once.
     */
    if (!wait_queue_init(&mutex->waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    mutex->owner = NULL;
    mutex->initialized = true;

    arch_irq_restore(flags);

    return true;
}


/* ============================================================
 * State query
 * ============================================================ */

bool mutex_is_locked(const mutex_t *mutex) {

    if (mutex == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool locked =
        mutex->initialized &&
        mutex->owner != NULL;

    arch_irq_restore(flags);

    return locked;
}


/* ============================================================
 * Non-blocking acquisition
 * ============================================================ */

bool mutex_try_lock(mutex_t *mutex) {

    if (mutex == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!mutex->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING ||
        current->scheduling_class == THREAD_CLASS_IDLE) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Reject recursive acquisition.
     */
    if (mutex->owner == current) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * An owned mutex cannot be acquired.
     */
    if (mutex->owner != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Do not bypass existing FIFO waiters.
     *
     * This preserves the mutex's handoff ordering.
     */
    if (!wait_queue_is_empty(&mutex->waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    mutex->owner = current;

    arch_irq_restore(flags);

    return true;
}


/* ============================================================
 * Blocking acquisition
 * ============================================================ */

bool mutex_lock(mutex_t *mutex) {

    if (mutex == NULL) {
        return false;
    }

    /*
     * Keep interrupts disabled while checking ownership
     * and entering the wait queue.
     *
     * This prevents an unlock from occurring between
     * observing an owned mutex and actually blocking.
     */
    uint64_t flags = arch_irq_save_disable();

    if (!mutex->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING ||
        current->scheduling_class == THREAD_CLASS_IDLE) {

        arch_irq_restore(flags);
        return false;
    }

    for (;;) {

        /*
         * Recursive acquisition is not supported.
         */
        if (mutex->owner == current) {

            arch_irq_restore(flags);
            return false;
        }

        /*
         * Acquire immediately if the mutex is free and
         * nobody is already waiting.
         */
        if (mutex->owner == NULL &&
            wait_queue_is_empty(&mutex->waiters)) {

            mutex->owner = current;

            arch_irq_restore(flags);

            return true;
        }

        /*
         * The mutex is owned or an earlier waiter has
         * priority. Block on its FIFO wait queue.
         *
         * thread_block() performs the BLOCKED transition
         * and queue insertion with interrupts disabled.
         *
         * Its nested IRQ-save operation preserves our
         * outer disabled-interrupt state across the switch.
         */
        if (!thread_block(&mutex->waiters)) {

            arch_irq_restore(flags);
            return false;
        }

        /*
         * We have resumed after being awakened.
         *
         * Unlock performs direct ownership handoff, so
         * this thread should now own the mutex.
         */
        if (mutex->owner == current) {

            arch_irq_restore(flags);
            return true;
        }

        /*
         * Defensive fallback:
         * if ownership was not handed to us, retry the
         * acquisition protocol while interrupts remain off.
         */
    }
}


/* ============================================================
 * Release
 * ============================================================ */

bool mutex_unlock(mutex_t *mutex) {

    if (mutex == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!mutex->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    /*
     * Only the owning thread may unlock.
     */
    if (current == NULL ||
        mutex->owner != current) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Validate the waiter queue before examining its head.
     */
    if (!wait_queue_validate(&mutex->waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * No waiters: release ownership completely.
     */
    if (wait_queue_is_empty(&mutex->waiters)) {

        mutex->owner = NULL;

        arch_irq_restore(flags);

        return true;
    }

    /*
     * Direct handoff:
     *
     * Assign ownership to the oldest waiter before
     * waking it. This prevents another thread from
     * acquiring the mutex ahead of that waiter.
     */
    struct thread *next_owner =
        mutex->waiters.head;

    if (next_owner == NULL ||
        next_owner->state != THREAD_STATE_BLOCKED ||
        next_owner->wait_channel != &mutex->waiters) {

        arch_irq_restore(flags);
        return false;
    }

    mutex->owner = next_owner;

    /*
     * Wake exactly the first FIFO waiter.
     *
     * If the scheduler cannot enqueue it, restore
     * ownership to the current thread. The wait queue
     * wake implementation restores the waiter to BLOCKED.
     */
    if (!wait_queue_wake_one(&mutex->waiters)) {

        mutex->owner = current;

        arch_irq_restore(flags);
        return false;
    }

    arch_irq_restore(flags);

    return true;
}
