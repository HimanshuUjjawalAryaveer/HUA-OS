#include <stdbool.h>
#include <stdint.h>

#include "scheduler/sleep_queue.h"
#include "arch/x86_64/irq_flags.h"


/* ============================================================
 * Internal validation
 * ============================================================ */

static bool sleep_queue_validate_locked(
    const struct scheduler_sleep_queue *queue
) {

    if (queue == NULL) {
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

    if (queue->head->sleep_previous != NULL ||
        queue->tail->sleep_next != NULL) {

        return false;
    }

    uint64_t visited = 0;

    struct thread *previous = NULL;
    struct thread *current = queue->head;

    uint64_t previous_deadline = 0;

    while (current != NULL) {

        if (visited >= queue->count) {
            return false;
        }

        if (current->sleep_previous != previous ||
            !current->sleep_queued ||
            current->state != THREAD_STATE_SLEEPING ||
            current->sleep_deadline_ns == 0) {

            return false;
        }

        /*
         * Sleeping threads cannot simultaneously belong
         * to a run queue or ordinary wait queue.
         */
        if (current->run_queue_owner != NULL ||
            current->run_previous != NULL ||
            current->run_next != NULL ||
            current->wait_channel != NULL ||
            current->wait_previous != NULL ||
            current->wait_next != NULL) {

            return false;
        }

        /*
         * A terminated thread cannot remain asleep.
         */
        if (current->reap_pending ||
            current->reap_queued ||
            current->reap_next != NULL) {

            return false;
        }

        /*
         * Deadlines must be in nondecreasing order.
         */
        if (visited != 0 &&
            current->sleep_deadline_ns < previous_deadline) {

            return false;
        }

        previous_deadline = current->sleep_deadline_ns;

        previous = current;
        current = current->sleep_next;

        visited++;
    }

    return visited == queue->count &&
           previous == queue->tail;
}


/* ============================================================
 * Initialization
 * ============================================================ */

void sleep_queue_init(
    struct scheduler_sleep_queue *queue
) {

    if (queue == NULL) {
        return;
    }

    uint64_t flags = arch_irq_save_disable();

    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;

    arch_irq_restore(flags);
}


/* ============================================================
 * Validation
 * ============================================================ */

bool sleep_queue_validate(
    const struct scheduler_sleep_queue *queue
) {

    if (queue == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool valid =
        sleep_queue_validate_locked(queue);

    arch_irq_restore(flags);

    return valid;
}


/* ============================================================
 * Insertion
 * ============================================================ */

bool sleep_queue_insert(
    struct scheduler_sleep_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!sleep_queue_validate_locked(queue)) {

        arch_irq_restore(flags);
        return false;
    }

    if (thread->state != THREAD_STATE_SLEEPING ||
        thread->sleep_queued ||
        thread->sleep_previous != NULL ||
        thread->sleep_next != NULL ||
        thread->sleep_deadline_ns == 0) {

        arch_irq_restore(flags);
        return false;
    }

    if (thread->run_queue_owner != NULL ||
        thread->run_previous != NULL ||
        thread->run_next != NULL ||
        thread->wait_channel != NULL ||
        thread->wait_previous != NULL ||
        thread->wait_next != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    if (thread->reap_pending ||
        thread->reap_queued ||
        thread->reap_next != NULL ||
        queue->count == UINT64_MAX) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Find the first thread with a strictly later deadline.
     *
     * Equal deadlines are inserted after existing entries.
     * This preserves FIFO ordering among equal deadlines.
     */
    struct thread *position = queue->head;

    while (position != NULL &&
           position->sleep_deadline_ns <=
               thread->sleep_deadline_ns) {

        position = position->sleep_next;
    }

    if (position == NULL) {

        /*
         * Insert at tail.
         */
        thread->sleep_previous = queue->tail;
        thread->sleep_next = NULL;

        if (queue->tail != NULL) {
            queue->tail->sleep_next = thread;
        } else {
            queue->head = thread;
        }

        queue->tail = thread;
    }

    else {

        /*
         * Insert before position.
         */
        thread->sleep_next = position;
        thread->sleep_previous =
            position->sleep_previous;

        if (position->sleep_previous != NULL) {

            position->sleep_previous->sleep_next =
                thread;

        } else {

            queue->head = thread;
        }

        position->sleep_previous = thread;
    }

    thread->sleep_queued = true;

    queue->count++;

    arch_irq_restore(flags);

    return true;
}


/* ============================================================
 * Removal
 * ============================================================ */

bool sleep_queue_remove(
    struct scheduler_sleep_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    if (!sleep_queue_validate_locked(queue)) {

        arch_irq_restore(flags);
        return false;
    }

    if (!thread->sleep_queued ||
        queue->count == 0) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Verify that this thread actually belongs to
     * the specified queue.
     */
    struct thread *current = queue->head;

    while (current != NULL && current != thread) {
        current = current->sleep_next;
    }

    if (current != thread) {

        arch_irq_restore(flags);
        return false;
    }

    if (thread->sleep_previous != NULL) {

        thread->sleep_previous->sleep_next =
            thread->sleep_next;

    } else {

        queue->head = thread->sleep_next;
    }

    if (thread->sleep_next != NULL) {

        thread->sleep_next->sleep_previous =
            thread->sleep_previous;

    } else {

        queue->tail = thread->sleep_previous;
    }

    thread->sleep_previous = NULL;
    thread->sleep_next = NULL;

    thread->sleep_queued = false;
    thread->sleep_deadline_ns = 0;

    queue->count--;

    arch_irq_restore(flags);

    return true;
}


/* ============================================================
 * Expiration
 * ============================================================ */


struct thread *sleep_queue_pop_expired(
    struct scheduler_sleep_queue *queue,
    uint64_t now_ns,
    uint64_t *expired_deadline_ns
) {

    if (queue == NULL || expired_deadline_ns == NULL) {
        return NULL;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * Perform bounded O(1) validation.
     *
     * Full queue validation remains available through
     * sleep_queue_validate() and normal queue operations.
     */
    if (queue->count == 0 ||
        queue->head == NULL ||
        queue->tail == NULL ||
        queue->head->sleep_previous != NULL ||
        queue->tail->sleep_next != NULL ||
        queue->head->sleep_queued == false ||
        queue->head->state != THREAD_STATE_SLEEPING ||
        queue->head->sleep_deadline_ns == 0 ||
        queue->head->sleep_deadline_ns > now_ns ||
        queue->head->run_queue_owner != NULL ||
        queue->head->run_previous != NULL ||
        queue->head->run_next != NULL ||
        queue->head->wait_channel != NULL ||
        queue->head->wait_previous != NULL ||
        queue->head->wait_next != NULL ||
        queue->head->reap_pending ||
        queue->head->reap_queued ||
        queue->head->reap_next != NULL) {

        arch_irq_restore(flags);
        return NULL;
    }

    struct thread *thread = queue->head;
    struct thread *next = thread->sleep_next;

    /*
     * Validate the immediate successor before unlinking.
     */
    if (next != NULL) {

        if (queue->count < 2 ||
            next == thread ||
            next->sleep_previous != thread ||
            !next->sleep_queued ||
            next->state != THREAD_STATE_SLEEPING ||
            next->sleep_deadline_ns == 0 ||
            next->sleep_deadline_ns < thread->sleep_deadline_ns) {

            arch_irq_restore(flags);
            return NULL;
        }

    } else {

        /*
         * A single-element queue must have matching
         * head and tail pointers and a count of one.
         */
        if (queue->tail != thread ||
            queue->count != 1) {

            arch_irq_restore(flags);
            return NULL;
        }
    }

    /*
     * Preserve the original deadline for possible rollback.
     */
    *expired_deadline_ns = thread->sleep_deadline_ns;

    /*
     * Remove the earliest expired sleeper.
     */
    queue->head = next;

    if (next != NULL) {
        next->sleep_previous = NULL;
    } else {
        queue->tail = NULL;
    }

    queue->count--;

    /*
     * Complete the sleep-to-ready transition.
     */
    thread->sleep_previous = NULL;
    thread->sleep_next = NULL;
    thread->sleep_queued = false;
    thread->sleep_deadline_ns = 0;
    thread->state = THREAD_STATE_READY;

    arch_irq_restore(flags);

    return thread;
}

/*
 * Restore a thread removed by sleep_queue_pop_expired()
 * when run-queue admission fails.
 *
 * The original thread was at the head, so restoring it
 * at the head preserves both deadline ordering and FIFO
 * ordering among equal deadlines.
 *
 * This operation is O(1).
 */
bool sleep_queue_restore_expired(
    struct scheduler_sleep_queue *queue,
    struct thread *thread,
    uint64_t deadline_ns
) {
    if (queue == NULL ||
        thread == NULL ||
        deadline_ns == 0) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * The popped thread must be READY and completely
     * detached from every scheduler-managed queue.
     */
    if (thread->state != THREAD_STATE_READY ||
        thread->sleep_previous != NULL ||
        thread->sleep_next != NULL ||
        thread->sleep_queued ||
        thread->sleep_deadline_ns != 0 ||
        thread->run_queue_owner != NULL ||
        thread->run_previous != NULL ||
        thread->run_next != NULL ||
        thread->wait_channel != NULL ||
        thread->wait_previous != NULL ||
        thread->wait_next != NULL ||
        thread->reap_pending ||
        thread->reap_queued ||
        thread->reap_next != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Validate the queue endpoints and count.
     */
    if (queue->count == UINT64_MAX ||
        ((queue->count == 0) !=
         (queue->head == NULL && queue->tail == NULL))) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * If the queue is not empty, the restored thread
     * must precede or equal the current head's deadline.
     */
    if (queue->count != 0) {

        struct thread *old_head = queue->head;

        if (old_head == NULL ||
            queue->tail == NULL ||
            old_head->sleep_previous != NULL ||
            queue->tail->sleep_next != NULL ||
            !old_head->sleep_queued ||
            old_head->state != THREAD_STATE_SLEEPING ||
            old_head->sleep_deadline_ns == 0 ||
            deadline_ns > old_head->sleep_deadline_ns) {

            arch_irq_restore(flags);
            return false;
        }

        /*
         * Verify that count, head, tail, and the immediate
         * successor agree.
         */
        if (queue->count == 1) {

            if (old_head != queue->tail ||
                old_head->sleep_next != NULL) {

                arch_irq_restore(flags);
                return false;
            }

        } else {

            struct thread *next = old_head->sleep_next;

            if (next == NULL ||
                next == old_head ||
                next->sleep_previous != old_head ||
                !next->sleep_queued ||
                next->state != THREAD_STATE_SLEEPING ||
                next->sleep_deadline_ns == 0 ||
                next->sleep_deadline_ns <
                    old_head->sleep_deadline_ns) {

                arch_irq_restore(flags);
                return false;
            }
        }

    } else if (queue->head != NULL || queue->tail != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Restore the original sleeper at the head.
     */
    thread->sleep_previous = NULL;
    thread->sleep_next = queue->head;
    thread->sleep_deadline_ns = deadline_ns;
    thread->sleep_queued = true;
    thread->state = THREAD_STATE_SLEEPING;

    if (queue->head != NULL) {
        queue->head->sleep_previous = thread;
    } else {
        queue->tail = thread;
    }

    queue->head = thread;
    queue->count++;

    arch_irq_restore(flags);

    return true;
}

/* ============================================================
 * Count
 * ============================================================ */

uint64_t sleep_queue_count(
    const struct scheduler_sleep_queue *queue
) {

    if (queue == NULL) {
        return 0;
    }

    uint64_t flags = arch_irq_save_disable();

    uint64_t count = queue->count;

    arch_irq_restore(flags);

    return count;
}
