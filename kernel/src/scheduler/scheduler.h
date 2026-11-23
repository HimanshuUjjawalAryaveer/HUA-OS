
#ifndef HUA_SCHEDULER_H
#define HUA_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler/thread.h"
#include "scheduler/sleep_queue.h"

#define SCHEDULER_MAX_CPUS 64U


/*
 * Basic intrusive FIFO queue.
 */
struct scheduler_thread_queue {
    struct thread *head;
    struct thread *tail;
    uint64_t count;
};


/*
 * Per-CPU runnable queues.
 *
 * Real-time:
 *     One FIFO queue for each fixed priority.
 *
 * Fair:
 *     Linked list, with minimum virtual deadline selected.
 *
 * Idle:
 *     Stored separately in scheduler_cpu.
 */
struct scheduler_run_queue {

    struct scheduler_thread_queue realtime[
        THREAD_RT_PRIORITY_LEVELS
    ];

    struct scheduler_thread_queue fair;

    /*
     * EEVDF fair scheduling aggregates.
     *
     * These values describe the fair threads currently
     * waiting in this run queue.
     */
    uint64_t fair_min_vruntime;

    uint64_t fair_avg_vruntime;

    uint64_t fair_total_weight;

    /*
     * Total runnable threads across all scheduling classes.
     */
    uint64_t runnable_count;
};


/*
 * Per-logical-CPU scheduler state.
 */
struct scheduler_cpu {

    bool initialized;

    uint32_t logical_id;

    /*
     * UINT32_MAX means the hardware APIC ID has not
     * yet been bound to this logical CPU.
     */
    uint32_t apic_id;

    struct thread *current_thread;
    struct thread *idle_thread;


    struct scheduler_run_queue run_queue;

    /*
    * Deadline-ordered sleeping threads.
    *
    * Sleeping threads are not runnable and therefore
    * are kept separately from the ordinary run queue.
    */
    struct scheduler_sleep_queue sleep_queue;

    /*
     * Deferred thread-reclamation list.
     *
     * Terminated threads remain here until another
     * thread can safely release their TCB and stack.
     */
    struct thread *reap_head;

    struct thread *reap_tail;

    uint64_t reap_count;

    /*
     * Scheduler diagnostics.
     */
    uint64_t context_switch_count;

    uint64_t voluntary_yield_count;

    bool reschedule_requested;

    uint64_t timer_ticks;

};


/*
 * Scheduler initialization.
 *
 * Registers the currently executing bootstrap thread,
 * creates the CPU0 idle thread, and initializes queues.
 *
 * Does not start scheduling or switch execution.
 */
bool scheduler_init(void);

bool scheduler_is_initialized(void);

struct scheduler_cpu *scheduler_get_cpu(
    uint32_t logical_id
);

struct thread *scheduler_get_current_thread(
    uint32_t logical_id
);


/*
 * Run-queue operations.
 */
void scheduler_run_queue_init(
    struct scheduler_run_queue *queue
);

bool scheduler_run_queue_enqueue(
    struct scheduler_run_queue *queue,
    struct thread *thread
);

bool scheduler_run_queue_dequeue(
    struct scheduler_run_queue *queue,
    struct thread *thread
);

/*
 * Returns and removes the next runnable thread.
 *
 * Returns NULL when no runnable thread exists.
 * The returned thread remains in THREAD_STATE_READY.
 */
struct thread *scheduler_run_queue_pick_next(
    struct scheduler_run_queue *queue
);


/*
 * CPU-specific queue operations.
 */
bool scheduler_enqueue_thread(
    uint32_t logical_id,
    struct thread *thread
);

bool scheduler_dequeue_thread(
    uint32_t logical_id,
    struct thread *thread
);

struct thread *scheduler_pick_runnable_thread(
    uint32_t logical_id
);

/*
 * Destroy terminated threads from the current CPU's reaper queue.
 *
 * Must only be called from ordinary thread context.
 */
void scheduler_reap_terminated_threads(void);

/*
 * Process a LAPIC timer tick at the interrupt-return boundary.
 *
 * Called from interrupt_dispatch() after the timer IRQ
 * handler has completed and acknowledged the interrupt.
 *
 * Accounts execution time and may perform a context switch
 * before returning to the interrupt stub.
 *
 * If the running thread has preemption disabled, a required
 * reschedule is recorded in reschedule_requested.
 *
 * That request is reevaluated at subsequent timer scheduling
 * boundaries after preemption protection is released.
 *
 * Does not execute deferred callbacks or reclaim terminated
 * threads.
 *
 * Current implementation: CPU0 only.
 *
 * The function must not enable interrupts.
 */
void scheduler_on_timer_interrupt_exit(void);

#endif
