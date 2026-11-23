#include "sync/semaphore.h"

#include "arch/x86_64/irq_flags.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>


/*
 * Initialize a counting semaphore.
 *
 * The wait queue and counter are initialized
 * before the semaphore becomes available.
 */
bool semaphore_init(
    semaphore_t *semaphore,
    uint64_t initial_count
) {

    if (semaphore == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (semaphore->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    if (!wait_queue_init(&semaphore->waiters)) {

        arch_irq_restore(flags);
        return false;
    }

    semaphore->count = initial_count;

    semaphore->initialized = true;

    arch_irq_restore(flags);

    return true;
}


/*
 * Acquire one semaphore permit.
 *
 * If permits are available, decrement the count.
 *
 * Otherwise, block the current thread.
 *
 * The IRQ-disabled region prevents a lost wakeup
 * between checking the counter and joining the
 * wait queue.
 */
bool semaphore_down(
    semaphore_t *semaphore
) {

    if (semaphore == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!semaphore->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * A permit is immediately available.
     */
    if (semaphore->count > 0) {

        semaphore->count--;

        arch_irq_restore(flags);

        return true;
    }

    /*
     * No permits are available.
     *
     * Block the current thread on the semaphore's
     * FIFO wait queue.
     *
     * thread_block() internally saves and restores
     * IRQ state. Because interrupts are already
     * disabled here, it restores them to disabled.
     *
     * Our outer IRQ state is restored afterward.
     */
    bool blocked =
        thread_block(&semaphore->waiters);

    if (!blocked) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * semaphore_up() wakes a waiter only when
     * directly transferring a released permit.
     *
     * Therefore, returning from thread_block()
     * means this thread has received one permit.
     */
    bool received_permit =
        scheduler_get_current_thread(0) == current;

    arch_irq_restore(flags);

    return received_permit;
}


/*
 * Attempt to acquire a permit without blocking.
 */
bool semaphore_try_down(
    semaphore_t *semaphore
) {

    if (semaphore == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!semaphore->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Do not bypass existing waiters.
     *
     * This preserves FIFO fairness.
     */
    if (semaphore->waiters.head != NULL ||
        semaphore->count == 0) {

        arch_irq_restore(flags);
        return false;
    }

    semaphore->count--;

    arch_irq_restore(flags);

    return true;
}


/*
 * Release one semaphore permit.
 *
 * If a thread is waiting, wake the first waiter.
 * The permit is transferred directly to that thread.
 *
 * Otherwise, increment the available permit count.
 */
bool semaphore_up(
    semaphore_t *semaphore
) {

    if (semaphore == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!semaphore->initialized) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Give the permit to an existing waiter.
     *
     * The waiter is removed from the wait queue
     * and moved to the runnable queue.
     */
    if (semaphore->waiters.head != NULL) {

        bool awakened =
            wait_queue_wake_one(&semaphore->waiters);

        arch_irq_restore(flags);

        return awakened;
    }

    /*
     * No waiters exist.
     *
     * Store the released permit.
     *
     * Prevent counter overflow.
     */
    if (semaphore->count == UINT64_MAX) {

        arch_irq_restore(flags);
        return false;
    }

    semaphore->count++;

    arch_irq_restore(flags);

    return true;
}


/*
 * Return the available permit count.
 */
uint64_t semaphore_get_count(
    const semaphore_t *semaphore
) {

    if (semaphore == NULL) {
        return 0;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!semaphore->initialized) {

        arch_irq_restore(flags);
        return 0;
    }

    uint64_t count = semaphore->count;

    arch_irq_restore(flags);

    return count;
}


/*
 * Check initialization state.
 */
bool semaphore_is_initialized(
    const semaphore_t *semaphore
) {

    if (semaphore == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool initialized = semaphore->initialized;

    arch_irq_restore(flags);

    return initialized;
}
