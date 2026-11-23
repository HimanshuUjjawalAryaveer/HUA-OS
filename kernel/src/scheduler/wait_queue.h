#ifndef HUA_WAIT_QUEUE_H
#define HUA_WAIT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler/thread.h"

/*
 * Intrusive FIFO wait queue.
 *
 * The queue does not own its threads.
 * Threads remain allocated while linked into the queue.
 *
 * A thread may belong to at most one wait queue.
 */
struct wait_queue {

    struct thread *head;
    struct thread *tail;

    uint64_t count;

    bool initialized;
};

/*
 * Initialize a wait queue.
 *
 * The queue storage must be zero-initialized before
 * its first initialization.
 *
 * A queue must never be reinitialized while it contains
 * waiting threads.
 */
bool wait_queue_init(struct wait_queue *queue);

/*
 * Check whether a queue has been initialized.
 */
bool wait_queue_is_initialized(
    const struct wait_queue *queue
);

/*
 * Check whether a queue contains no threads.
 */
bool wait_queue_is_empty(
    const struct wait_queue *queue
);

/*
 * Return the number of waiting threads.
 *
 * Returns zero for an invalid or uninitialized queue.
 */
uint64_t wait_queue_count(
    const struct wait_queue *queue
);

/*
 * Validate the queue's complete linked-list structure.
 */
bool wait_queue_validate(
    const struct wait_queue *queue
);

/*
 * Append a BLOCKED thread to the end of a wait queue.
 *
 * Does not change the thread's state.
 *
 * The caller must transition the thread to BLOCKED
 * while protecting the complete blocking operation.
 */
bool wait_queue_enqueue(
    struct wait_queue *queue,
    struct thread *thread
);

/*
 * Remove a particular thread from its wait queue.
 *
 * Does not change the thread's state.
 */
bool wait_queue_remove(
    struct wait_queue *queue,
    struct thread *thread
);

/*
 * Wake the first thread in FIFO order.
 *
 * Returns true if a thread was successfully awakened.
 */
bool wait_queue_wake_one(
    struct wait_queue *queue
);

/*
 * Wake as many waiting threads as possible.
 *
 * Returns the number of successfully awakened threads.
 */
uint64_t wait_queue_wake_all(
    struct wait_queue *queue
);

#endif
