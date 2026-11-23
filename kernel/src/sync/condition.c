#include "sync/condition.h"

#include "arch/x86_64/irq_flags.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"

#include <stddef.h>
#include <stdint.h>


/* ============================================================
 * Initialization
 * ============================================================ */

bool condition_init(condition_t *condition) {

    if (condition == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (condition->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    if (!wait_queue_init(&condition->waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    condition->initialized = true;

    arch_irq_restore(flags);

    return true;
}


/* ============================================================
 * State queries
 * ============================================================ */

bool condition_is_initialized(
    const condition_t *condition
) {

    if (condition == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool initialized = condition->initialized;

    arch_irq_restore(flags);

    return initialized;
}


uint64_t condition_waiter_count(
    const condition_t *condition
) {

    if (condition == NULL) {
        return 0;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!condition->initialized) {

        arch_irq_restore(flags);
        return 0;
    }

    uint64_t count =
        wait_queue_count(&condition->waiters);

    arch_irq_restore(flags);

    return count;
}


/* ============================================================
 * Wait
 * ============================================================ */

/*
 * Release the supplied mutex, block on the condition
 * variable, and reacquire the mutex before returning.
 *
 * Interrupts remain disabled across the release-to-block
 * transition. This prevents a lost notification on CPU0.
 */
bool condition_wait(
    condition_t *condition,
    mutex_t *mutex
) {

    if (condition == NULL ||
        mutex == NULL) {

        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * Validate both objects.
     */
    if (!condition->initialized ||
        !mutex->initialized ||
        !wait_queue_validate(&condition->waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * The caller must be a normal running thread.
     */
    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING ||
        current->scheduling_class == THREAD_CLASS_IDLE ||
        current->preempt_count != 0) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Waiting is only valid when the caller owns
     * the supplied mutex.
     */
    if (mutex->owner != current) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Release the mutex while interrupts remain disabled.
     *
     * This may hand ownership directly to an existing
     * mutex waiter.
     */
    if (!mutex_unlock(mutex)) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Atomically transition this thread to BLOCKED
     * and insert it into the condition wait queue.
     *
     * The outer IRQ-disabled state remains in effect.
     */
    bool blocked =
        thread_block(&condition->waiters);

    if (!blocked) {

        /*
         * Blocking failed.
         *
         * Attempt to restore the caller's mutex ownership
         * before returning.
         *
         * mutex_lock() may itself need to block if another
         * thread acquired the mutex in the meantime.
         */
        bool reacquired = mutex_lock(mutex);

        (void)reacquired;

        arch_irq_restore(flags);

        return false;
    }

    /*
     * A signal or broadcast woke this thread.
     *
     * Reacquire the original mutex before returning.
     *
     * This may block if another thread currently owns it.
     * The outer interrupt-disabled state remains preserved.
     */
    bool reacquired = mutex_lock(mutex);

    arch_irq_restore(flags);

    return reacquired;
}


/* ============================================================
 * Signal
 * ============================================================ */

/*
 * Wake the oldest waiting thread.
 *
 * A signal with no waiters is a successful no-op.
 */
bool condition_signal(
    condition_t *condition
) {

    if (condition == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!condition->initialized ||
        !wait_queue_validate(&condition->waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Nothing to wake.
     */
    if (wait_queue_is_empty(&condition->waiters)) {

        arch_irq_restore(flags);
        return true;
    }

    /*
     * Wake exactly one FIFO waiter.
     */
    bool result =
        wait_queue_wake_one(&condition->waiters);

    arch_irq_restore(flags);

    return result;
}


/* ============================================================
 * Broadcast
 * ============================================================ */

/*
 * Wake all currently waiting threads.
 *
 * The wait queue processes threads in FIFO order.
 */
uint64_t condition_broadcast(
    condition_t *condition
) {

    if (condition == NULL) {
        return 0;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!condition->initialized ||
        !wait_queue_validate(&condition->waiters)) {

        arch_irq_restore(flags);
        return 0;
    }

    uint64_t awakened =
        wait_queue_wake_all(&condition->waiters);

    arch_irq_restore(flags);

    return awakened;
}
