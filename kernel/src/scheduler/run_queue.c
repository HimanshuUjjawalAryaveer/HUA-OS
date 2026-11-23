
#include "scheduler/run_queue.h"
#include "scheduler/fair.h"

/*
 * Initialize one FIFO queue.
 */
static void scheduler_thread_queue_init(
    struct scheduler_thread_queue *queue
) {

    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
}


/*
 * Append a thread to a FIFO queue.
 */
static bool scheduler_thread_queue_append(
    struct scheduler_thread_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    if (thread->run_previous != NULL ||
        thread->run_next != NULL) {

        return false;
    }

    if (queue->count == UINT64_MAX) {
        return false;
    }

    thread->run_previous = queue->tail;
    thread->run_next = NULL;

    if (queue->tail != NULL) {
        queue->tail->run_next = thread;
    } else {
        queue->head = thread;
    }

    queue->tail = thread;
    queue->count++;

    return true;
}


/*
 * Remove a particular thread from a FIFO queue.
 */
static bool scheduler_thread_queue_remove(
    struct scheduler_thread_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL ||
        queue->count == 0) {

        return false;
    }

    /*
     * Verify membership before changing any pointers.
     */
    struct thread *current = queue->head;

    while (current != NULL && current != thread) {
        current = current->run_next;
    }

    if (current == NULL) {
        return false;
    }

    if (thread->run_previous != NULL) {
        thread->run_previous->run_next =
            thread->run_next;
    } else {
        queue->head = thread->run_next;
    }

    if (thread->run_next != NULL) {
        thread->run_next->run_previous =
            thread->run_previous;
    } else {
        queue->tail = thread->run_previous;
    }

    thread->run_previous = NULL;
    thread->run_next = NULL;

    queue->count--;

    return true;
}


/*
 * Initialize every queue belonging to one CPU.
 */
void scheduler_run_queue_init(
    struct scheduler_run_queue *queue
) {

    if (queue == NULL) {
        return;
    }

    for (uint32_t i = 0;
         i < THREAD_RT_PRIORITY_LEVELS;
         i++) {

        scheduler_thread_queue_init(
            &queue->realtime[i]
        );
    }

    scheduler_thread_queue_init(&queue->fair);

    queue->fair_min_vruntime = 0;
    queue->fair_avg_vruntime = 0;
    queue->fair_total_weight = 0;

    queue->runnable_count = 0;
}


/*
 * Insert a thread into the appropriate scheduling class.
 */
bool scheduler_run_queue_enqueue(
    struct scheduler_run_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    if (thread->scheduling_class == THREAD_CLASS_IDLE) {
        return false;
    }

    if (thread->run_queue_owner != NULL) {
        return false;
    }

    if (thread->run_previous != NULL ||
        thread->run_next != NULL) {

        return false;
    }

    if (thread->state != THREAD_STATE_NEW &&
        thread->state != THREAD_STATE_READY &&
        thread->state != THREAD_STATE_RUNNING) {

        return false;
    }

    /*
    * A runnable thread must not retain membership in
    * any other scheduler-managed queue.
    */

    /* Wait-queue membership must be completely detached. */
    if (thread->wait_previous != NULL ||
        thread->wait_next != NULL ||
        thread->wait_channel != NULL) {

        return false;
    }

    /* Sleep-queue membership and deadline must be cleared. */
    if (thread->sleep_previous != NULL ||
        thread->sleep_next != NULL ||
        thread->sleep_queued ||
        thread->sleep_deadline_ns != 0) {

        return false;
    }

    /* A terminated or pending-reclamation thread cannot run. */
    if (thread->reap_next != NULL ||
        thread->reap_pending ||
        thread->reap_queued) {

        return false;
    }

    if (queue->runnable_count == UINT64_MAX) {
        return false;
    }

    struct scheduler_thread_queue *destination = NULL;

    switch (thread->scheduling_class) {

        case THREAD_CLASS_REALTIME:

            if (thread->realtime_priority >=
                THREAD_RT_PRIORITY_LEVELS) {

                return false;
            }

            destination =
                &queue->realtime[
                    thread->realtime_priority
                ];

            break;

        case THREAD_CLASS_FAIR:

            if (thread->fair_weight <
                    THREAD_FAIR_WEIGHT_MIN ||
                thread->fair_weight >
                    THREAD_FAIR_WEIGHT_MAX) {

                return false;
            }

            if (queue->fair_total_weight >
                SCHEDULER_FAIR_MAX_RUNQUEUE_WEIGHT -
                thread->fair_weight) {

                return false;
            }

            /*
            * A newly created or otherwise READY fair thread
            * receives a deadline for its scheduling request.
            *
            * A RUNNING thread being re-enqueued must preserve
            * its existing deadline unless its scheduler path
            * explicitly renews the request.
            *
            * This is important when an RT thread interrupts
            * a fair thread before its fair slice expires.
            */
            if (thread->state != THREAD_STATE_RUNNING) {

                if (!scheduler_fair_update_deadline(thread)) {
                    return false;
                }
            }

            destination = &queue->fair;

            break;

        default:
            return false;
    }

    if (!scheduler_thread_queue_append(
            destination,
            thread
        )) {

        return false;
    }

    thread->run_queue_owner = queue;
    thread->state = THREAD_STATE_READY;

    queue->runnable_count++;

    if (thread->scheduling_class == THREAD_CLASS_FAIR) {
        scheduler_fair_recalculate(queue);
    }

    return true;
}


/*
 * Remove a thread from the run queue that owns it.
 */
bool scheduler_run_queue_dequeue(
    struct scheduler_run_queue *queue,
    struct thread *thread
) {

    if (queue == NULL || thread == NULL) {
        return false;
    }

    if (thread->run_queue_owner != queue) {
        return false;
    }

    if (thread->state != THREAD_STATE_READY) {
        return false;
    }

    struct scheduler_thread_queue *source = NULL;

    switch (thread->scheduling_class) {

        case THREAD_CLASS_REALTIME:

            if (thread->realtime_priority >=
                THREAD_RT_PRIORITY_LEVELS) {

                return false;
            }

            source =
                &queue->realtime[
                    thread->realtime_priority
                ];

            break;

        case THREAD_CLASS_FAIR:

            source = &queue->fair;
            break;

        default:
            return false;
    }

    if (queue->runnable_count == 0) {
        return false;
    }

    if (!scheduler_thread_queue_remove(
            source,
            thread
        )) {

        return false;
    }

    thread->run_queue_owner = NULL;

    queue->runnable_count--;

    if (thread->scheduling_class == THREAD_CLASS_FAIR) {
        scheduler_fair_recalculate(queue);
    }

    return true;
}


/*
 * Pick the next runnable thread.
 *
 * Policy:
 * 1. Highest real-time priority first.
 * 2. FIFO order within the same real-time priority.
 * 3. Fair thread with the smallest virtual deadline.
 *
 * The idle thread is deliberately not part of this queue.
 */
struct thread *scheduler_run_queue_pick_next(
    struct scheduler_run_queue *queue
) {

    if (queue == NULL || queue->runnable_count == 0) {
        return NULL;
    }

    struct thread *selected = NULL;

    /*
     * Real-time priorities:
     * 31 is highest; 0 is lowest.
     */
    for (uint32_t priority =
             THREAD_RT_PRIORITY_LEVELS;
         priority > 0;
         priority--) {

        struct scheduler_thread_queue *rt_queue =
            &queue->realtime[priority - 1];

        if (rt_queue->head != NULL) {
            selected = rt_queue->head;
            break;
        }
    }

    /*
     * If no real-time thread is runnable,
     * choose the best fair thread.
     */
    if (selected == NULL) {
        selected = scheduler_fair_pick(queue);
    }

    if (selected == NULL) {
        return NULL;
    }

    if (!scheduler_run_queue_dequeue(queue, selected)) {
        return NULL;
    }

    return selected;
}
