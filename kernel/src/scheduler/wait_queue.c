#include <stdbool.h>
#include <stdint.h>

#include "scheduler/wait_queue.h"
#include "arch/x86_64/irq_flags.h"
#include "scheduler/scheduler.h"

/*
 * Interrupt protection.
 *
 * HUA currently runs on CPU0.
 *
 * Disabling local interrupts protects wait queues from
 * concurrent modification by interrupt handlers.
 *
 * This is not an SMP lock.
 */


/* ============================================================
 * Internal validation
 * ============================================================ */

/*
 * Validate a queue while local interrupts are disabled.
 *
 * Checks:
 * - Empty queue consistency.
 * - Head and tail consistency.
 * - Correct previous links.
 * - Correct wait-channel ownership.
 * - Every member is BLOCKED.
 * - No member is simultaneously runnable.
 * - Traversal count matches queue->count.
 */
static bool wait_queue_validate_locked(
    const struct wait_queue *queue
) {

    if (queue == NULL || !queue->initialized) {
        return false;
    }

    if (queue->count == 0) {

        return queue->head == NULL &&
               queue->tail == NULL;
    }

    if (queue->head == NULL ||
        queue->tail == NULL) {

        return false;
    }

    if (queue->head->wait_previous != NULL ||
        queue->tail->wait_next != NULL) {

        return false;
    }

    uint64_t visited = 0;

    struct thread *previous = NULL;
    struct thread *current = queue->head;

    while (current != NULL) {

        /*
         * More nodes than the recorded count indicates
         * a cycle or corrupted queue metadata.
         */
        if (visited >= queue->count) {
            return false;
        }

        if (current->wait_previous != previous ||
            current->wait_channel != queue ||
            current->state != THREAD_STATE_BLOCKED) {

            return false;
        }

        /*
         * A blocked thread must not also be runnable.
         */
        if (current->run_queue_owner != NULL ||
            current->run_previous != NULL ||
            current->run_next != NULL) {

            return false;
        }

        /*
        * A blocked thread must not simultaneously belong
        * to a timed-sleep queue.
        */
        if (current->sleep_previous != NULL ||
            current->sleep_next != NULL ||
            current->sleep_queued ||
            current->sleep_deadline_ns != 0) {

            return false;
        }

        /*
         * Terminated threads must never remain
         * in a wait queue.
         */
        if (current->reap_pending ||
            current->reap_queued ||
            current->reap_next != NULL) {

            return false;
        }

        previous = current;
        current = current->wait_next;

        visited++;
    }

    /*
     * The final visited thread must be the recorded tail.
     */
    return visited == queue->count &&
           previous == queue->tail;
}


/* ============================================================
 * Initialization
 * ============================================================ */

bool wait_queue_init(
    struct wait_queue *queue
) {

    if (queue == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * Reject repeated initialization.
     *
     * Queue storage must have been zero-initialized
     * before the first call.
     */
    if (queue->initialized) {

        arch_irq_restore(flags);

        return false;
    }

    if (queue->head != NULL ||
        queue->tail != NULL ||
        queue->count != 0) {

        arch_irq_restore(flags);

        return false;
    }

    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;

    queue->initialized = true;

    arch_irq_restore(flags);

    return true;
}


/* ============================================================
 * State queries
 * ============================================================ */

bool wait_queue_is_initialized(
    const struct wait_queue *queue
) {

    if (queue == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool initialized = queue->initialized;

    arch_irq_restore(flags);

    return initialized;
}


bool wait_queue_is_empty(
    const struct wait_queue *queue
) {

    if (queue == NULL) {
        return true;
    }

    uint64_t flags = arch_irq_save_disable();

    bool empty =
        queue->initialized &&
        queue->count == 0 &&
        queue->head == NULL &&
        queue->tail == NULL;

    arch_irq_restore(flags);

    return empty;
}


uint64_t wait_queue_count(
    const struct wait_queue *queue
) {

    if (queue == NULL) {
        return 0;
    }

    uint64_t flags = arch_irq_save_disable();

    uint64_t count = 0;

    if (queue->initialized) {
        count = queue->count;
    }

    arch_irq_restore(flags);

    return count;
}


bool wait_queue_validate(
    const struct wait_queue *queue
) {

    if (queue == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool valid =
        wait_queue_validate_locked(queue);

    arch_irq_restore(flags);

    return valid;
}


/* ============================================================
 * Insertion
 * ============================================================ */

bool wait_queue_enqueue(
    struct wait_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * Validate the existing queue before modifying it.
     */
    if (!wait_queue_validate_locked(queue)) {

        arch_irq_restore(flags);

        return false;
    }

    /*
     * Only BLOCKED threads belong in ordinary wait queues.
     */
    if (thread->state != THREAD_STATE_BLOCKED) {

        arch_irq_restore(flags);

        return false;
    }

    /*
     * Reject existing wait-queue membership.
     */
    if (thread->wait_channel != NULL ||
        thread->wait_previous != NULL ||
        thread->wait_next != NULL) {

        arch_irq_restore(flags);

        return false;
    }

    /*
     * A blocked thread cannot simultaneously belong
     * to a runnable queue.
     */
    if (thread->run_queue_owner != NULL ||
        thread->run_previous != NULL ||
        thread->run_next != NULL) {

        arch_irq_restore(flags);

        return false;
    }

    /*
    * A thread already associated with a timed sleep
    * cannot enter an ordinary wait queue.
    */
    if (thread->sleep_previous != NULL ||
        thread->sleep_next != NULL ||
        thread->sleep_queued ||
        thread->sleep_deadline_ns != 0) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Reject threads awaiting reclamation.
     */
    if (thread->reap_pending ||
        thread->reap_queued ||
        thread->reap_next != NULL) {

        arch_irq_restore(flags);

        return false;
    }

    if (queue->count == UINT64_MAX) {

        arch_irq_restore(flags);

        return false;
    }

    /*
     * Append at the tail.
     *
     * This provides FIFO wakeup order.
     */
    thread->wait_previous = queue->tail;
    thread->wait_next = NULL;

    thread->wait_channel = queue;

    if (queue->tail != NULL) {

        queue->tail->wait_next = thread;

    } else {

        queue->head = thread;
    }

    queue->tail = thread;

    queue->count++;

    arch_irq_restore(flags);

    return true;
}


/* ============================================================
 * Removal
 * ============================================================ */

bool wait_queue_remove(
    struct wait_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * Refuse to modify a corrupted queue.
     */
    if (!wait_queue_validate_locked(queue)) {

        arch_irq_restore(flags);

        return false;
    }

    /*
     * Ownership identifies the queue containing this thread.
     */
    if (thread->wait_channel != queue ||
        queue->count == 0) {

        arch_irq_restore(flags);

        return false;
    }

    /*
     * Remove from the previous neighbor or update head.
     */
    if (thread->wait_previous != NULL) {

        thread->wait_previous->wait_next =
            thread->wait_next;

    } else {

        queue->head = thread->wait_next;
    }

    /*
     * Remove from the next neighbor or update tail.
     */
    if (thread->wait_next != NULL) {

        thread->wait_next->wait_previous =
            thread->wait_previous;

    } else {

        queue->tail = thread->wait_previous;
    }

    /*
     * Clear all membership information.
     */
    thread->wait_previous = NULL;
    thread->wait_next = NULL;
    thread->wait_channel = NULL;

    queue->count--;

    arch_irq_restore(flags);

    return true;
}

/* ============================================================
 * Thread wakeup
 * ============================================================ */

/*
 * Wake one thread while local interrupts are disabled.
 *
 * The caller must hold the interrupt-protected critical
 * section covering the wait queue and scheduler run queue.
 */
static bool wait_queue_wake_locked(
    struct wait_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        thread->state != THREAD_STATE_BLOCKED ||
        thread->wait_channel != queue ||
        thread->scheduling_class == THREAD_CLASS_IDLE ||
        (thread->affinity_mask & 1ULL) == 0) {

        return false;
    }

    /*
     * Remove the thread from its wait queue.
     */
    if (!wait_queue_remove(queue, thread)) {
        return false;
    }

    /*
     * The run queue accepts READY threads.
     */
    thread->state = THREAD_STATE_READY;

    /*
     * Insert into CPU0's runnable queue.
     */
    if (!scheduler_run_queue_enqueue(
            &cpu->run_queue,
            thread
        )) {

        /*
         * Roll back the transition if enqueue fails.
         *
         * The thread returns to BLOCKED state and is
         * reinserted into its original wait queue.
         */
        thread->state = THREAD_STATE_BLOCKED;

        if (!wait_queue_enqueue(queue, thread)) {

            /*
             * Failure to restore queue membership means
             * scheduler state is no longer trustworthy.
             */
            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt" ::: "memory");
            }
        }

        return false;
    }

    /*
     * The timer scheduler will reevaluate whether this
     * newly runnable thread should preempt the current one.
     */
    cpu->reschedule_requested = true;

    return true;
}


/*
 * Wake a specific blocked thread.
 */
bool thread_wake(
    struct thread *thread
) {

    if (thread == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    struct wait_queue *queue =
        (struct wait_queue *)thread->wait_channel;

    if (queue == NULL ||
        !wait_queue_validate_locked(queue)) {

        arch_irq_restore(flags);
        return false;
    }

    bool result =
        wait_queue_wake_locked(queue, thread);

    arch_irq_restore(flags);

    return result;
}


/*
 * Wake the first waiting thread.
 *
 * FIFO order is maintained because new waiters are
 * inserted at the tail.
 */
bool wait_queue_wake_one(
    struct wait_queue *queue
) {

    if (queue == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!wait_queue_validate_locked(queue) ||
        queue->head == NULL) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *thread = queue->head;

    bool result =
        wait_queue_wake_locked(queue, thread);

    arch_irq_restore(flags);

    return result;
}


/*
 * Wake all threads currently waiting in the queue.
 *
 * Threads are processed from head to tail.
 *
 * If a wakeup fails, the remaining threads stay blocked.
 */
uint64_t wait_queue_wake_all(
    struct wait_queue *queue
) {

    if (queue == NULL) {
        return 0;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!wait_queue_validate_locked(queue)) {

        arch_irq_restore(flags);
        return 0;
    }

    uint64_t awakened = 0;

    while (queue->head != NULL) {

        struct thread *thread = queue->head;

        if (!wait_queue_wake_locked(queue, thread)) {
            break;
        }

        awakened++;
    }

    arch_irq_restore(flags);

    return awakened;
}
