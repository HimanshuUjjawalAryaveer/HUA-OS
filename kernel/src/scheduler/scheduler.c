#include "scheduler/scheduler.h"
#include "scheduler/fair.h"
#include "scheduler/wait_queue.h"
#include "arch/x86_64/irq_flags.h"
#include "arch/x86_64/context_switch.h"
#include "time/time.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "process/process.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/syscall.h"

/*
 * Scheduler state for all supported logical CPUs.
 *
 * Only CPU0 is initialized at this stage.
 */
static struct scheduler_cpu scheduler_cpus[
    SCHEDULER_MAX_CPUS
];

static bool scheduler_initialized = false;

/*
 * Activate the architecture state required by a selected thread.
 *
 * This includes:
 *
 * 1. CR3: the selected thread's address space.
 * 2. TSS.RSP0: the selected thread's kernel privilege-transition stack.
 *
 * Process-owned threads use their cached borrowed address-space
 * pointer. Their process reference guarantees its lifetime.
 *
 * Process-independent threads use the permanent kernel address space.
 *
 * Threads with owned stacks use their own stack_top.
 *
 * The bootstrap thread has a borrowed stack with unknown bounds,
 * so it uses the dedicated bootstrap transition stack.
 *
 * Called with interrupts disabled at scheduler context-switch
 * boundaries. Any failure is fatal because execution must never
 * continue with inconsistent architectural state.
 */
static void scheduler_activate_thread_arch_state_or_halt(
    struct scheduler_cpu *cpu,
    struct thread *thread
) {

    if (cpu == NULL ||
        thread == NULL ||
        !cpu->initialized ||
        cpu->logical_id >= SCHEDULER_MAX_CPUS) {

        goto activation_failure;
    }

    /*
     * ---------------------------------------------------------
     * 1. Validate the selected thread's kernel stack.
     * ---------------------------------------------------------
     */

    uintptr_t kernel_stack_top = 0;

    if (thread->stack_owned) {

        /*
         * An owned stack must have complete, internally
         * consistent metadata.
         */
        if (thread->stack_base == NULL ||
            thread->stack_size < THREAD_STACK_MIN_SIZE ||
            thread->stack_top == 0) {

            goto activation_failure;
        }

        uintptr_t stack_base =
            (uintptr_t)thread->stack_base;

        /*
         * Check arithmetic before calculating the expected
         * address. This prevents integer overflow.
         */
        if (thread->stack_size >
            UINTPTR_MAX - stack_base) {

            goto activation_failure;
        }

        uintptr_t expected_stack_top =
            stack_base + thread->stack_size;

        /*
         * The recorded top must exactly match the allocation.
         */
        if (thread->stack_top != expected_stack_top ||
            (expected_stack_top & 0xFULL) != 0) {

            goto activation_failure;
        }

        kernel_stack_top = expected_stack_top;

    } else {

        /*
         * HUA's bootstrap thread has an externally provided
         * stack with unknown bounds.
         *
         * Do not fabricate stack metadata for it.
         */
        if (thread->stack_base != NULL ||
            thread->stack_size != 0 ||
            thread->stack_top != 0) {

            goto activation_failure;
        }
    }

    /*
     * ---------------------------------------------------------
     * 2. Resolve the address space required by this thread.
     * ---------------------------------------------------------
     */

    vmm_address_space_t *address_space = NULL;

    if (thread->process == NULL) {

        /*
         * Process-independent kernel and scheduler threads.
         */
        if (thread->address_space != NULL) {
            goto activation_failure;
        }

        address_space = vmm_kernel_address_space();

    } else {

        /*
         * Process-owned thread.
         *
         * The cached pointer is protected by the process
         * reference held by this thread.
         */
        if (thread->address_space == NULL) {
            goto activation_failure;
        }

        address_space = thread->address_space;
    }

    if (address_space == NULL) {
        goto activation_failure;
    }

    /*
     * ---------------------------------------------------------
     * 3. Configure the next Ring 3 -> Ring 0 kernel stack.
     * ---------------------------------------------------------
     *
     * Do this before transferring execution.
     */

    bool stack_updated = false;

    if (thread->stack_owned) {

        stack_updated =
            gdt_set_kernel_stack(kernel_stack_top);

    } else {

        /*
         * Restore the dedicated bootstrap transition stack.
         */
        stack_updated =
            gdt_restore_bootstrap_stack();
    }

    if (!stack_updated) {
        goto activation_failure;
    }

    /*
    * Synchronize the trusted syscall-entry stack
    * with the hardware TSS.RSP0.
    */
    uintptr_t active_kernel_stack_top = 0;

    if (!gdt_get_kernel_stack(&active_kernel_stack_top)) {
        goto activation_failure;
    }

    if (!arch_syscall_set_kernel_stack(active_kernel_stack_top)) {
        goto activation_failure;
    }

    /*
     * ---------------------------------------------------------
     * 4. Activate the selected address space through CR3.
     * ---------------------------------------------------------
     */

    if (!vmm_address_space_activate(
            cpu->logical_id,
            address_space
        )) {

        goto activation_failure;
    }

    return;

activation_failure:

    /*
     * Continuing with the wrong kernel stack or address space
     * could cause memory corruption or a privilege-transition
     * failure.
     *
     * Fail closed.
     */
    asm volatile ("cli" ::: "memory");

    for (;;) {
        asm volatile ("hlt" ::: "memory");
    }
}

/*
 * Idle thread entry.
 *
 * It performs no work. It waits for interrupts and will
 * eventually be the fallback when no runnable thread exists.
 *
 * This function is prepared now but is not executed yet.
 */
static void scheduler_idle_entry(void *argument) {

    (void)argument;

    for (;;) {

        /*
        * Sleep until an interrupt arrives.
        */
        asm volatile ("sti; hlt" ::: "memory");

        /*
        * An interrupt may have made another thread runnable.
        * Give the scheduler an opportunity to select it.
        */
        (void)thread_yield();
    }
}


/*
 * Initialize the scheduler.
 *
 * This function does not enable scheduling.
 * It only establishes CPU0's initial scheduler state.
 */
bool scheduler_init(void) {

    if (scheduler_initialized) {
        return false;
    }

    /*
     * Initialize CPU0's run queues.
     */
    struct scheduler_cpu *cpu = &scheduler_cpus[0];

    scheduler_run_queue_init(&cpu->run_queue);
    sleep_queue_init(&cpu->sleep_queue);

    /*
     * Initialize deferred-reclamation metadata.
     */
    cpu->reap_head = NULL;
    cpu->reap_tail = NULL;
    cpu->reap_count = 0;

    /*
     * Initialize scheduler diagnostics.
     */
    cpu->context_switch_count = 0;
    cpu->voluntary_yield_count = 0;

    /*
     * Adopt the current kmain execution as the bootstrap
     * thread. Its Limine stack remains borrowed.
     */
    struct thread *bootstrap =
        thread_adopt_bootstrap("bootstrap");

    if (bootstrap == NULL) {
        return false;
    }

    /*
     * Create the CPU0 idle thread.
     */
    struct thread *idle = thread_create(
        "idle/0",
        scheduler_idle_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (idle == NULL) {

        /*
         * The bootstrap TCB can be released, but its
         * borrowed stack must never be freed.
         */
        bootstrap->state = THREAD_STATE_NEW;
        thread_destroy(bootstrap);

        return false;
    }

    /*
     * The idle thread is stored separately.
     * It must never enter a normal run queue.
     */
    idle->scheduling_class = THREAD_CLASS_IDLE;
    idle->state = THREAD_STATE_READY;

    /*
     * Publish CPU0's initial scheduler state.
     */
    cpu->logical_id = 0;

    /*
     * APIC identity binding will be added when we connect
     * scheduler CPU records to the LAPIC CPU enumeration.
     */
    cpu->apic_id = UINT32_MAX;

    cpu->current_thread = bootstrap;
    cpu->idle_thread = idle;

    cpu->reschedule_requested = false;
    cpu->timer_ticks = 0;

    cpu->initialized = true;

    scheduler_initialized = true;

    return true;
}


/*
 * Check whether scheduler initialization completed.
 */
bool scheduler_is_initialized(void) {
    return scheduler_initialized;
}


/*
 * Return an initialized CPU scheduler structure.
 */
struct scheduler_cpu *scheduler_get_cpu(
    uint32_t logical_id
) {

    if (!scheduler_initialized ||
        logical_id >= SCHEDULER_MAX_CPUS) {

        return NULL;
    }

    struct scheduler_cpu *cpu =
        &scheduler_cpus[logical_id];

    if (!cpu->initialized) {
        return NULL;
    }

    return cpu;
}


/*
 * Return the thread currently recorded as running
 * on the requested logical CPU.
 */
struct thread *scheduler_get_current_thread(
    uint32_t logical_id
) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(logical_id);

    if (cpu == NULL) {
        return NULL;
    }

    return cpu->current_thread;
}

/*
 * Check whether a runnable RT thread has a priority
 * strictly higher than the currently running RT thread.
 *
 * Priority 31 is the highest.
 */
static bool scheduler_rt_has_higher_priority(
    const struct scheduler_run_queue *queue,
    uint8_t current_priority
) {

    if (queue == NULL ||
        current_priority >= THREAD_RT_PRIORITY_LEVELS) {

        return false;
    }

    for (uint32_t priority =
             THREAD_RT_PRIORITY_LEVELS;
         priority > (uint32_t)current_priority + 1U;
         priority--) {

        if (queue->realtime[priority - 1U].head != NULL) {
            return true;
        }
    }

    return false;
}


/*
 * Check whether an equal-priority RT peer is waiting.
 */
static bool scheduler_rt_has_equal_priority_peer(
    const struct scheduler_run_queue *queue,
    uint8_t current_priority
) {

    if (queue == NULL ||
        current_priority >= THREAD_RT_PRIORITY_LEVELS) {

        return false;
    }

    return queue->realtime[current_priority].head != NULL;
}

/*
 * Maximum number of sleepers promoted to READY during
 * one timer interrupt.
 *
 * This bounds work performed in interrupt context.
 */
#define SCHEDULER_SLEEP_WAKE_BUDGET 64U


/*
 * Promote expired sleepers into CPU0's runnable queue.
 *
 * This function:
 * - Does not allocate memory.
 * - Does not execute callbacks.
 * - Does not destroy threads.
 * - Does not perform context switches.
 *
 * The ordinary scheduler policy decides whether the
 * current thread should be preempted afterward.
 */
static void scheduler_wake_expired_sleepers(
    struct scheduler_cpu *cpu,
    uint64_t now_ns
) {

    if (cpu == NULL) {
        return;
    }

    for (uint32_t count = 0;
         count < SCHEDULER_SLEEP_WAKE_BUDGET;
         count++) {

        uint64_t deadline_ns = 0;

        struct thread *thread = sleep_queue_pop_expired(
            &cpu->sleep_queue,
            now_ns,
            &deadline_ns
        );

        if (thread == NULL) {
            break;
        }

        /*
        * sleep_queue_pop_expired() has already detached the
        * thread and changed it to READY.
        *
        * Save its original deadline before attempting to
        * insert it into the runnable queue.
        */

        if (!scheduler_run_queue_enqueue(&cpu->run_queue, thread)) {

            /*
            * Run-queue admission failed.
            *
            * Restore the sleeper at its original position:
            * the head of the deadline-ordered sleep queue.
            */
            if (!sleep_queue_restore_expired(
                    &cpu->sleep_queue,
                    thread,
                    deadline_ns)) {

                /*
                * Queue recovery failed. Continuing could
                * leave scheduler state inconsistent.
                */
                asm volatile ("cli" ::: "memory");

                for (;;) {
                    asm volatile ("hlt" ::: "memory");
                }
            }

            break;
        }

        /*
         * A newly runnable thread may deserve the CPU.
         * The normal timer scheduling policy will decide.
         */
        cpu->reschedule_requested = true;
    }
}

void scheduler_on_timer_interrupt_exit(void) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL) {
        return;
    }

    if (cpu->timer_ticks != UINT64_MAX) {
        cpu->timer_ticks++;
    }

    struct thread *current = cpu->current_thread;

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING) {

        cpu->reschedule_requested = false;
        return;
    }

    /*
     * Account elapsed execution time.
     *
     * The timer interrupt runs with interrupts disabled.
     * No other thread can modify this CPU's running-thread
     * accounting while this function executes.
     */
    uint64_t now_ns = 0;

    bool time_valid =
        time_get_monotonic_ns(&now_ns) == TIME_STATUS_OK;

    /*
    * Expire timed sleepers before making the scheduling
    * decision for this timer tick.
    */
    if (time_valid) {

        scheduler_wake_expired_sleepers(
            cpu,
            now_ns
        );
    }

    bool accounting_ok = false;

    if (time_valid) {

        if (current->last_start_ns == 0) {
            current->last_start_ns = now_ns;
        }

        if (now_ns >= current->last_start_ns) {

            uint64_t elapsed_ns =
                now_ns - current->last_start_ns;

            accounting_ok = true;

            if (elapsed_ns != 0) {

                if (current->scheduling_class ==
                    THREAD_CLASS_FAIR) {

                    accounting_ok =
                        scheduler_fair_account_runtime(
                            current,
                            elapsed_ns
                        );
                }

                else {

                    if (current->runtime_ns >
                        UINT64_MAX - elapsed_ns) {

                        accounting_ok = false;
                    }

                    else {

                        current->runtime_ns += elapsed_ns;
                    }
                }
            }

            if (accounting_ok) {

                if (current->slice_runtime_ns >
                    UINT64_MAX - elapsed_ns) {

                    current->slice_runtime_ns = UINT64_MAX;
                }

                else {

                    current->slice_runtime_ns += elapsed_ns;
                }

                current->last_start_ns = now_ns;
            }
        }
    }

    /*
     * Scheduling decision state.
     */
    bool higher_priority_waiting = false;
    bool equal_priority_waiting = false;

    bool should_preempt = false;
    bool should_rotate_rr = false;
    bool should_rotate_fair = false;

    /*
     * Determine whether a scheduling-class transition
     * or a time-slice rotation is required.
     */
    switch (current->scheduling_class) {

        case THREAD_CLASS_IDLE:

            /*
             * Idle must give the CPU to any runnable thread.
             */
            should_preempt =
                cpu->run_queue.runnable_count != 0;

            break;


        case THREAD_CLASS_FAIR: {

            /*
             * Real-time work always outranks fair work.
             */
            bool realtime_waiting = false;

            for (uint32_t priority = 0;
                 priority < THREAD_RT_PRIORITY_LEVELS;
                 priority++) {

                if (cpu->run_queue.realtime[priority].head != NULL) {

                    realtime_waiting = true;
                    break;
                }
            }

            /*
             * Check whether the fair request has expired.
             *
             * This check uses accumulated slice runtime,
             * so it remains meaningful even if the latest
             * clock read failed.
             */
            bool fair_slice_expired =
                scheduler_fair_slice_expired(current);

            /*
             * If the fair request has expired, renew it
             * when this thread is actually switched out.
             *
             * This also applies when RT work causes the
             * switch. The fair thread must not resume with
             * an already-consumed request.
             */
            should_rotate_fair = fair_slice_expired;

            /*
             * RT preemption has priority over fair rotation.
             *
             * Otherwise, an expired fair request gives
             * another runnable fair thread an opportunity.
             */
            should_preempt =
                realtime_waiting ||
                scheduler_fair_should_preempt(
                    current,
                    &cpu->run_queue
                );

            break;
        }


        case THREAD_CLASS_REALTIME:

            if (current->realtime_priority >=
                THREAD_RT_PRIORITY_LEVELS) {

                cpu->reschedule_requested = false;
                return;
            }

            /*
             * Check for strictly higher-priority RT work.
             */
            higher_priority_waiting =
                scheduler_rt_has_higher_priority(
                    &cpu->run_queue,
                    current->realtime_priority
                );

            if (higher_priority_waiting) {

                should_preempt = true;
            }

            /*
             * Equal-priority rotation is allowed only
             * for round-robin threads.
             *
             * FIFO threads do not rotate automatically.
             */
            else if (
                current->realtime_policy ==
                    THREAD_RT_ROUND_ROBIN &&
                current->realtime_quantum_ns != 0 &&
                accounting_ok &&
                current->slice_runtime_ns >=
                    current->realtime_quantum_ns
            ) {

                equal_priority_waiting =
                    scheduler_rt_has_equal_priority_peer(
                        &cpu->run_queue,
                        current->realtime_priority
                    );

                if (equal_priority_waiting) {

                    should_preempt = true;
                    should_rotate_rr = true;
                }
            }

            break;


        default:

            cpu->reschedule_requested = false;
            return;
    }

    /*
     * No scheduling decision is necessary.
     */
    if (!should_preempt) {

        cpu->reschedule_requested = false;
        return;
    }

    /*
     * Respect nested preemption protection.
     *
     * Do not change queues or reset a scheduling request
     * while preemption is disabled.
     */
    if (current->preempt_count != 0) {

        cpu->reschedule_requested = true;
        return;
    }

    /*
     * If a fair request expired, calculate its next
     * virtual deadline before re-enqueuing it.
     *
     * If RT interrupted it before expiration, this block
     * is skipped and its existing deadline is preserved.
     */
    if (should_rotate_fair) {

        if (!scheduler_fair_update_deadline(current)) {

            cpu->reschedule_requested = true;
            return;
        }
    }

    /*
     * Re-enqueue the outgoing thread.
     *
     * Idle is not part of the ordinary run queue.
     */
    if (current->scheduling_class != THREAD_CLASS_IDLE) {

        if (!scheduler_run_queue_enqueue(
                &cpu->run_queue,
                current
            )) {

            cpu->reschedule_requested = true;
            return;
        }

    } else {

        /*
        * Idle is not stored in the ordinary run queue.
        * When another thread takes the CPU, idle becomes
        * READY so it can be selected again later.
        */
        current->state = THREAD_STATE_READY;
    }

    /*
     * Select the next runnable thread.
     *
     * The run queue enforces:
     * 1. Highest RT priority.
     * 2. EEVDF fair selection.
     */
    struct thread *selected =
        scheduler_run_queue_pick_next(&cpu->run_queue);

    /*
     * An empty queue after a scheduling decision is
     * unexpected because the outgoing non-idle thread
     * was just inserted.
     *
     * Restore the outgoing thread safely.
     */
    if (selected == NULL) {

        if (current->scheduling_class != THREAD_CLASS_IDLE) {

            if (!scheduler_run_queue_dequeue(
                    &cpu->run_queue,
                    current
                )) {

                asm volatile ("cli" ::: "memory");

                for (;;) {
                    asm volatile ("hlt" ::: "memory");
                }
            }
        }

        current->state = THREAD_STATE_RUNNING;
        cpu->reschedule_requested = true;

        return;
    }

    /*
     * EEVDF may legitimately select the same fair thread
     * again if it remains eligible and has the earliest
     * virtual deadline.
     *
     * It has already been removed from the queue by
     * scheduler_run_queue_pick_next().
     *
     * Do not attempt to dequeue it a second time.
     */
    if (selected == current) {

        current->state = THREAD_STATE_RUNNING;
        current->last_start_ns = time_valid ? now_ns : 0;

        if (should_rotate_fair || should_rotate_rr) {
            current->slice_runtime_ns = 0;
        }

        cpu->reschedule_requested = false;

        return;
    }

    /*
     * Reset the outgoing scheduling request only when
     * its quantum/request has actually expired.
     *
     * Higher-priority RT preemption of an RR thread
     * preserves its remaining quantum.
     *
     * RT preemption of an unexpired fair request also
     * preserves its remaining slice.
     */
    if (should_rotate_rr || should_rotate_fair) {
        current->slice_runtime_ns = 0;
    }

    /*
    * Activate the selected thread's address space before
    * publishing it as the CPU's current thread.
    */
    scheduler_activate_thread_arch_state_or_halt(cpu, selected);

    /*
    * Activate the selected thread.
    */
    selected->state = THREAD_STATE_RUNNING;
    selected->last_cpu = cpu->logical_id;
    selected->last_start_ns = time_valid ? now_ns : 0;

    cpu->current_thread = selected;

    cpu->reschedule_requested = false;

    if (cpu->context_switch_count != UINT64_MAX) {
        cpu->context_switch_count++;
    }

    /*
     * Switch directly from the interrupt-dispatch stack.
     *
     * When the outgoing thread is selected again, its
     * suspended interrupt-dispatch execution resumes here.
     *
     * Do not:
     * - Call thread_yield().
     * - Enable interrupts.
     * - Run deferred callbacks.
     * - Reclaim terminated threads.
     */
    arch_context_switch(
        &current->context,
        &selected->context
    );
}


/*
 * Enqueue a thread on a specific CPU.
 */
bool scheduler_enqueue_thread(
    uint32_t logical_id,
    struct thread *thread
) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(logical_id);

    if (cpu == NULL || thread == NULL) {
        return false;
    }

    if (thread->scheduling_class == THREAD_CLASS_IDLE) {
        return false;
    }

    if (logical_id >= 64 ||
        (thread->affinity_mask & (1ULL << logical_id)) == 0) {

        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool result = scheduler_run_queue_enqueue(
        &cpu->run_queue,
        thread
    );

    arch_irq_restore(flags);

    return result;
}


/*
 * Remove a thread from a specific CPU's run queue.
 */
bool scheduler_dequeue_thread(
    uint32_t logical_id,
    struct thread *thread
) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(logical_id);

    if (cpu == NULL || thread == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool result = scheduler_run_queue_dequeue(
        &cpu->run_queue,
        thread
    );

    arch_irq_restore(flags);

    return result;
}


/*
 * Select the next runnable thread on a CPU.
 *
 * Returns NULL when no ordinary runnable thread exists.
 * The caller must then consider the CPU's idle thread.
 *
 * This function does not change current_thread and does
 * not perform a context switch.
 */
struct thread *scheduler_pick_runnable_thread(
    uint32_t logical_id
) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(logical_id);

    if (cpu == NULL) {
        return NULL;
    }

    uint64_t flags = arch_irq_save_disable();

    struct thread *selected =
        scheduler_run_queue_pick_next(
            &cpu->run_queue
        );

    arch_irq_restore(flags);

    return selected;
}


/*
 * Voluntarily schedule another runnable thread.
 *
 * This is a cooperative dispatcher:
 *
 * 1. Disable interrupts and preserve their previous state.
 * 2. Account runtime consumed by the current thread.
 * 3. Select another runnable thread BEFORE re-enqueuing
 *    the outgoing thread.
 * 4. Put the outgoing thread back into its run queue.
 * 5. Update CPU scheduler state.
 * 6. Switch architecture contexts.
 * 7. Restore the original interrupt state when this
 *    thread is eventually scheduled again.
 *
 * This function must never be called from an IRQ handler.
 */
bool thread_yield(void) {

    uint64_t flags = arch_irq_save_disable();

    /*
     * Only CPU0 is active in the current implementation.
     */
    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL) {
        arch_irq_restore(flags);
        return false;
    }

    struct thread *outgoing = cpu->current_thread;

    if (outgoing == NULL ||
        outgoing->state != THREAD_STATE_RUNNING ||
        outgoing->preempt_count != 0) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Read monotonic time while scheduler state is protected.
     */
    uint64_t now_ns = 0;

    if (time_get_monotonic_ns(&now_ns) != TIME_STATUS_OK) {
        arch_irq_restore(flags);
        return false;
    }

    /*
     * The first dispatch establishes a runtime baseline.
     */
    if (outgoing->last_start_ns == 0) {
        outgoing->last_start_ns = now_ns;
    }

    if (now_ns < outgoing->last_start_ns) {
        arch_irq_restore(flags);
        return false;
    }

    uint64_t elapsed_ns =
        now_ns - outgoing->last_start_ns;

    /*
     * Check slice accounting before modifying runtime
     * or virtual runtime. This avoids partial accounting
     * if the slice counter would overflow.
     */
    if (outgoing->slice_runtime_ns >
        UINT64_MAX - elapsed_ns) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Account actual execution time.
     *
     * Fair accounting updates both runtime_ns and
     * virtual_runtime. Other scheduling classes only
     * need ordinary runtime accounting at this stage.
     */
    if (elapsed_ns != 0) {

        if (outgoing->scheduling_class == THREAD_CLASS_FAIR) {

            if (!scheduler_fair_account_runtime(
                    outgoing,
                    elapsed_ns
                )) {

                arch_irq_restore(flags);
                return false;
            }

        } else {

            if (outgoing->runtime_ns >
                UINT64_MAX - elapsed_ns) {

                arch_irq_restore(flags);
                return false;
            }

            outgoing->runtime_ns += elapsed_ns;
        }
    }

    outgoing->slice_runtime_ns += elapsed_ns;

    /*
     * IMPORTANT:
     *
     * Select the next thread BEFORE re-enqueuing the
     * outgoing thread.
     *
     * Otherwise, the outgoing thread could immediately
     * win selection and effectively yield to itself.
     */
    struct thread *selected =
        scheduler_run_queue_pick_next(&cpu->run_queue);

    /*
     * Nobody else is runnable.
     *
     * Continue executing the current thread rather than
     * switching unnecessarily to the idle thread.
     *
     * However, terminated threads may still be waiting
     * for deferred reclamation. A successful yield that
     * does not switch must still make cleanup progress.
     *
     * We are in ordinary thread context, not interrupt
     * context, so reclamation is permitted.
     */
    if (selected == NULL) {

        outgoing->last_start_ns = now_ns;

        if (cpu->voluntary_yield_count != UINT64_MAX) {
            cpu->voluntary_yield_count++;
        }

        /*
         * Drain terminated threads even though no
         * context switch is necessary.
         *
         * The reaper protects itself against involuntary
         * preemption and preserves the current IRQ state.
         */
        scheduler_reap_terminated_threads();

        arch_irq_restore(flags);

        return true;
    }

    /*
    * A successful voluntary yield ends the current
    * fair scheduling request.
    *
    * Renew its virtual deadline using its newly
    * accounted virtual runtime.
    */
    if (outgoing->scheduling_class == THREAD_CLASS_FAIR) {

        if (!scheduler_fair_update_deadline(outgoing)) {

            /*
            * The selected thread was already removed
            * from the run queue. Restore it before
            * returning.
            */
            if (!scheduler_run_queue_enqueue(
                    &cpu->run_queue,
                    selected
                )) {

                for (;;) {
                    asm volatile ("cli; hlt" ::: "memory");
                }
            }

            arch_irq_restore(flags);

            return false;
        }
    }

    if (outgoing->scheduling_class != THREAD_CLASS_IDLE) {

        if (!scheduler_run_queue_enqueue(
                &cpu->run_queue,
                outgoing
            )) {

            /*
            * Restore the selected thread to the queue
            * if outgoing insertion fails.
            */
            if (!scheduler_run_queue_enqueue(
                    &cpu->run_queue,
                    selected
                )) {

                /*
                * Queue recovery failed. This indicates
                * corrupted scheduler state.
                */
                for (;;) {
                    asm volatile ("cli; hlt" ::: "memory");
                }
            }

            arch_irq_restore(flags);
            return false;
        }

        /*
        * A voluntary yield ends this thread's current slice.
        * It will receive a fresh slice when dispatched again.
        */
        outgoing->slice_runtime_ns = 0;

    } else {

        /*
        * The idle thread is not part of the ordinary
        * runnable queue, but it must become READY when
        * it switches to another thread.
        */
        outgoing->state = THREAD_STATE_READY;
    }

    /*
    * Activate the selected thread's address space before
    * publishing it as the CPU's current thread.
    */
    scheduler_activate_thread_arch_state_or_halt(cpu, selected);

    /*
    * The selected thread was removed from its run queue.
    * It is now the running thread.
    */
    selected->state = THREAD_STATE_RUNNING;
    selected->last_cpu = cpu->logical_id;
    selected->last_start_ns = now_ns;

    /*
     * Publish the new CPU execution state before switching
     * stacks. Interrupts remain disabled.
     */
    cpu->current_thread = selected;

    if (cpu->context_switch_count != UINT64_MAX) {
        cpu->context_switch_count++;
    }

    if (cpu->voluntary_yield_count != UINT64_MAX) {
        cpu->voluntary_yield_count++;
    }

    /*
     * Save the outgoing context and restore the selected
     * thread's context.
     *
     * When this outgoing thread is selected again in the
     * future, execution resumes here.
     */
    arch_context_switch(
        &outgoing->context,
        &selected->context
    );

    /*
    * We have resumed this thread on its own stack.
    * It is now safe to reclaim previously terminated threads.
    */
    scheduler_reap_terminated_threads();

    /*
     * We have resumed this thread.
     *
     * Its own saved flags are still in this suspended
     * invocation's stack frame.
     *
     * Restore the interrupt state that existed before
     * this thread originally called thread_yield().
     */
    arch_irq_restore(flags);

    return true;
}

/*
 * Block the currently running thread on a wait queue.
 *
 * Unlike thread_yield(), the outgoing thread is NOT
 * reinserted into the runnable queue.
 *
 * Its context remains saved until a wakeup operation
 * makes it READY and the scheduler eventually selects it.
 *
 * Current implementation: CPU0 only.
 */
bool thread_block(
    struct wait_queue *queue
) {

    if (queue == NULL) {
        return false;
    }

    /*
     * Keep interrupts disabled throughout:
     * - runtime accounting
     * - transition to BLOCKED
     * - wait-queue insertion
     * - next-thread selection
     * - context switch
     *
     * This prevents a wakeup from racing with the
     * blocking transition.
     */
    uint64_t flags = arch_irq_save_disable();

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        cpu->current_thread == NULL) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *outgoing =
        cpu->current_thread;

    /*
     * Only an ordinary running thread may block.
     */
    if (outgoing->state != THREAD_STATE_RUNNING ||
        outgoing->scheduling_class == THREAD_CLASS_IDLE ||
        outgoing->preempt_count != 0 ||
        outgoing->run_queue_owner != NULL ||
        outgoing->run_previous != NULL ||
        outgoing->run_next != NULL ||
        outgoing->wait_channel != NULL ||
        outgoing->wait_previous != NULL ||
        outgoing->wait_next != NULL ||
        outgoing->sleep_queued ||
        outgoing->sleep_previous != NULL ||
        outgoing->sleep_next != NULL ||
        outgoing->sleep_deadline_ns != 0 ||
        outgoing->reap_pending ||
        outgoing->reap_queued ||
        outgoing->reap_next != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Read monotonic time before changing scheduler state.
     */
    uint64_t now_ns = 0;

    if (time_get_monotonic_ns(&now_ns) != TIME_STATUS_OK) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Establish the initial runtime baseline if necessary.
     */
    if (outgoing->last_start_ns == 0) {
        outgoing->last_start_ns = now_ns;
    }

    if (now_ns < outgoing->last_start_ns) {

        arch_irq_restore(flags);
        return false;
    }

    uint64_t elapsed_ns =
        now_ns - outgoing->last_start_ns;

    /*
     * Validate accounting before modifying it.
     */
    if (outgoing->slice_runtime_ns >
        UINT64_MAX - elapsed_ns) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Account the execution interval ending at the
     * moment this thread blocks.
     */
    if (elapsed_ns != 0) {

        if (outgoing->scheduling_class ==
            THREAD_CLASS_FAIR) {

            if (!scheduler_fair_account_runtime(
                    outgoing,
                    elapsed_ns
                )) {

                arch_irq_restore(flags);
                return false;
            }

        } else {

            if (outgoing->runtime_ns >
                UINT64_MAX - elapsed_ns) {

                arch_irq_restore(flags);
                return false;
            }

            outgoing->runtime_ns += elapsed_ns;
        }
    }

    outgoing->slice_runtime_ns += elapsed_ns;

    /*
     * Mark BLOCKED before insertion because the wait queue
     * accepts only BLOCKED threads.
     *
     * Interrupts are still disabled, so no wakeup can
     * observe this intermediate state.
     */
    outgoing->state = THREAD_STATE_BLOCKED;

    if (!wait_queue_enqueue(queue, outgoing)) {

        outgoing->state = THREAD_STATE_RUNNING;

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Select another ordinary runnable thread.
     *
     * The blocked thread is not in the run queue.
     */
    struct thread *selected =
        scheduler_run_queue_pick_next(
            &cpu->run_queue
        );

    /*
     * If no ordinary thread is runnable, execute the
     * CPU's idle thread.
     */
    if (selected == NULL) {

        selected = cpu->idle_thread;

        if (selected == NULL ||
            selected == outgoing ||
            (selected->state != THREAD_STATE_READY &&
            selected->state != THREAD_STATE_RUNNING) ||
            selected->scheduling_class != THREAD_CLASS_IDLE) {

            /*
             * Restore the original state if dispatch
             * cannot safely proceed.
             */
            if (!wait_queue_remove(queue, outgoing)) {

                asm volatile ("cli" ::: "memory");

                for (;;) {
                    asm volatile ("hlt" ::: "memory");
                }
            }

            outgoing->state = THREAD_STATE_RUNNING;

            arch_irq_restore(flags);
            return false;
        }
    }

    /*
    * Activate the selected thread's address space.
    */
    scheduler_activate_thread_arch_state_or_halt(cpu, selected);

    /*
    * Activate the selected thread.
    */
    selected->state = THREAD_STATE_RUNNING;
    selected->last_cpu = cpu->logical_id;
    selected->last_start_ns = now_ns;

    /*
     * Publish scheduler state before switching stacks.
     */
    cpu->current_thread = selected;

    if (cpu->context_switch_count != UINT64_MAX) {
        cpu->context_switch_count++;
    }

    /*
     * Switch away from the blocked thread.
     *
     * This invocation remains suspended on the outgoing
     * thread's own stack.
     *
     * It resumes only after a wakeup and a later dispatch.
     */
    arch_context_switch(
        &outgoing->context,
        &selected->context
    );

    /*
     * The blocked thread has now been woken and selected.
     *
     * Its stack is active again. It is safe to reclaim
     * previously terminated threads.
     */
    scheduler_reap_terminated_threads();

    /*
     * Restore the interrupt state saved before blocking.
     */
    arch_irq_restore(flags);

    return true;
}

/*
 * Sleep the currently running thread until an absolute
 * monotonic-clock deadline.
 *
 * The thread is removed from CPU execution and placed
 * in CPU0's deadline-ordered sleep queue.
 */
bool thread_sleep_until(
    uint64_t deadline_ns
) {

    if (deadline_ns == 0) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        cpu->current_thread == NULL) {

        arch_irq_restore(flags);
        return false;
    }

    struct thread *outgoing =
        cpu->current_thread;

    /*
     * Only an ordinary running thread may sleep.
     */
    if (outgoing->state != THREAD_STATE_RUNNING ||
        outgoing->scheduling_class == THREAD_CLASS_IDLE ||
        outgoing->preempt_count != 0 ||
        outgoing->run_queue_owner != NULL ||
        outgoing->run_previous != NULL ||
        outgoing->run_next != NULL ||
        outgoing->wait_channel != NULL ||
        outgoing->wait_previous != NULL ||
        outgoing->wait_next != NULL ||
        outgoing->sleep_queued ||
        outgoing->sleep_previous != NULL ||
        outgoing->sleep_next != NULL ||
        outgoing->sleep_deadline_ns != 0 ||
        outgoing->reap_pending ||
        outgoing->reap_queued ||
        outgoing->reap_next != NULL) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Read the monotonic clock.
     */
    uint64_t now_ns = 0;

    if (time_get_monotonic_ns(&now_ns) != TIME_STATUS_OK) {

        arch_irq_restore(flags);
        return false;
    }

    /*
     * No sleep is necessary if the deadline has passed.
     */
    if (deadline_ns <= now_ns) {

        arch_irq_restore(flags);
        return true;
    }

    /*
     * Account the execution interval ending now.
     */
    if (outgoing->last_start_ns == 0) {
        outgoing->last_start_ns = now_ns;
    }

    if (now_ns < outgoing->last_start_ns) {

        arch_irq_restore(flags);
        return false;
    }

    uint64_t elapsed_ns =
        now_ns - outgoing->last_start_ns;

    if (outgoing->slice_runtime_ns >
        UINT64_MAX - elapsed_ns) {

        arch_irq_restore(flags);
        return false;
    }

    if (elapsed_ns != 0) {

        if (outgoing->scheduling_class ==
            THREAD_CLASS_FAIR) {

            if (!scheduler_fair_account_runtime(
                    outgoing,
                    elapsed_ns
                )) {

                arch_irq_restore(flags);
                return false;
            }

        } else {

            if (outgoing->runtime_ns >
                UINT64_MAX - elapsed_ns) {

                arch_irq_restore(flags);
                return false;
            }

            outgoing->runtime_ns += elapsed_ns;
        }
    }

    outgoing->slice_runtime_ns += elapsed_ns;

    /*
     * Transition to SLEEPING before queue insertion.
     * Interrupts remain disabled during the transition.
     */
    outgoing->state = THREAD_STATE_SLEEPING;
    outgoing->sleep_deadline_ns = deadline_ns;

    if (!sleep_queue_insert(
            &cpu->sleep_queue,
            outgoing
        )) {

        outgoing->state = THREAD_STATE_RUNNING;
        outgoing->sleep_deadline_ns = 0;

        arch_irq_restore(flags);
        return false;
    }

    /*
     * Select another runnable thread.
     */
    struct thread *selected =
        scheduler_run_queue_pick_next(
            &cpu->run_queue
        );

    /*
     * Fall back to CPU0's idle thread.
     */
    if (selected == NULL) {

        selected = cpu->idle_thread;

        if (selected == NULL ||
            selected == outgoing ||
            selected->state != THREAD_STATE_READY ||
            selected->scheduling_class != THREAD_CLASS_IDLE) {

            if (!sleep_queue_remove(
                    &cpu->sleep_queue,
                    outgoing
                )) {

                asm volatile ("cli" ::: "memory");

                for (;;) {
                    asm volatile ("hlt" ::: "memory");
                }
            }

            outgoing->state = THREAD_STATE_RUNNING;

            arch_irq_restore(flags);
            return false;
        }
    }

    /*
    * Activate the selected thread's address space.
    */
    scheduler_activate_thread_arch_state_or_halt(cpu, selected);

    /*
    * Activate the selected thread.
    */
    selected->state = THREAD_STATE_RUNNING;
    selected->last_cpu = cpu->logical_id;
    selected->last_start_ns = now_ns;

    cpu->current_thread = selected;

    if (cpu->context_switch_count != UINT64_MAX) {
        cpu->context_switch_count++;
    }

    /*
     * Switch away from the sleeping thread.
     *
     * The call resumes only after timer expiration has
     * made this thread READY and the scheduler selects it.
     */
    arch_context_switch(
        &outgoing->context,
        &selected->context
    );

    /*
     * The sleeping thread has resumed.
     */
    scheduler_reap_terminated_threads();

    arch_irq_restore(flags);

    return true;
}


/*
 * Sleep for a relative duration.
 *
 * Convert the duration to an absolute monotonic deadline
 * using checked arithmetic.
 */
bool thread_sleep(
    uint64_t duration_ns
) {

    if (duration_ns == 0) {
        return true;
    }

    uint64_t now_ns = 0;

    if (time_get_monotonic_ns(&now_ns) != TIME_STATUS_OK) {
        return false;
    }

    if (duration_ns > UINT64_MAX - now_ns) {
        return false;
    }

    uint64_t deadline_ns =
        now_ns + duration_ns;

    return thread_sleep_until(deadline_ns);
}

/*
 * Reclaim terminated threads.
 *
 * A terminated thread cannot free its own stack because
 * execution is still using that stack.
 *
 * Therefore, another running thread performs reclamation.
 *
 * The complete operation is protected against involuntary
 * preemption because thread_destroy() releases memory through
 * the shared kernel heap.
 *
 * Current implementation:
 * - CPU0 only.
 * - Single-core preemption protection.
 * - No SMP synchronization.
 */
void scheduler_reap_terminated_threads(void) {

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL) {
        return;
    }

    /*
     * Prevent another thread from running while we detach
     * terminated threads and release their memory.
     *
     * Heap operations performed by thread_destroy() acquire
     * additional nested protection levels.
     */
    if (!thread_preempt_disable()) {
        return;
    }

    for (;;) {

        uint64_t flags = arch_irq_save_disable();

        struct thread *thread = cpu->reap_head;

        /*
         * No more terminated threads need reclamation.
         */
        if (thread == NULL) {

            arch_irq_restore(flags);

            break;
        }

        /*
         * Validate the reaper queue entry before detaching it.
         */
        if (thread == cpu->current_thread ||
            thread->state != THREAD_STATE_TERMINATED ||
            !thread->reap_pending ||
            !thread->reap_queued ||
            cpu->reap_count == 0) {

            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt");
            }
        }

        /*
         * Remove the entry from the reaper queue.
         */
        cpu->reap_head = thread->reap_next;

        if (cpu->reap_head == NULL) {
            cpu->reap_tail = NULL;
        }

        cpu->reap_count--;

        /*
         * The thread is no longer linked into the queue.
         *
         * Keep reap_pending set until thread_destroy()
         * validates and destroys it.
         */
        thread->reap_next = NULL;
        thread->reap_queued = false;

        arch_irq_restore(flags);

        /*
         * We are executing on a different thread's stack.
         *
         * If this thread belonged to a process, account
         * for its permanent termination now.
         *
         * This is deliberately performed here rather than
         * inside thread_exit(), because the reaper runs from
         * a different thread's stack.
         */
        if (thread->process != NULL) {

            if (thread->address_space == NULL ||
                !process_thread_exit(
                    thread->process,
                    thread
                )) {

                asm volatile ("cli" ::: "memory");

                for (;;) {
                    asm volatile ("hlt" ::: "memory");
                }
            }

            /*
            * process_thread_exit() removed the TCB from the
            * process list and cleared its borrowed ownership.
            */

        } else if (thread->address_space != NULL) {

            /*
            * A process-independent thread must not retain
            * a process address-space pointer.
            */
            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt" ::: "memory");
            }
        }

        /*
         * Release the terminated thread's stack and TCB.
         *
         * Preemption remains disabled throughout this call,
         * including the nested heap operations.
         */
        if (!thread_destroy(thread)) {

            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt");
            }
        }
    }

    /*
     * Release the reaper's preemption protection.
     *
     * If a timer interrupt requested rescheduling while
     * reclamation was in progress, the scheduler will
     * reevaluate that request at its next timer boundary.
     */
    if (!thread_preempt_enable()) {

        asm volatile ("cli" ::: "memory");

        for (;;) {
            asm volatile ("hlt");
        }
    }
}

/*
 * Permanently terminate the currently running thread.
 *
 * The current thread:
 * 1. Accounts for its final runtime.
 * 2. Is marked TERMINATED.
 * 3. Is added to the CPU's reaper queue.
 * 4. Is never re-enqueued.
 * 5. Switches to another runnable thread, or to the idle thread.
 *
 * This function must never be called from an interrupt handler.
 * It must never return.
 */
_Noreturn void thread_exit(int64_t exit_code) {

    (void)arch_irq_save_disable();

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL || !scheduler_initialized) {
        for (;;) {
            asm volatile ("cli; hlt" ::: "memory");
        }
    }

    struct thread *outgoing = cpu->current_thread;

    /*
     * The idle thread is a permanent scheduler thread.
     * It must not terminate through this path.
     */
    if (outgoing == NULL ||
        outgoing == cpu->idle_thread ||
        outgoing->scheduling_class == THREAD_CLASS_IDLE ||
        outgoing->state != THREAD_STATE_RUNNING ||
        outgoing->preempt_count != 0 ||
        outgoing->run_queue_owner != NULL ||
        outgoing->run_previous != NULL ||
        outgoing->run_next != NULL ||
        outgoing->wait_previous != NULL ||
        outgoing->wait_next != NULL ||
        outgoing->wait_channel != NULL ||
        outgoing->sleep_queued ||
        outgoing->sleep_previous != NULL ||
        outgoing->sleep_next != NULL ||
        outgoing->sleep_deadline_ns != 0 ||
        outgoing->reap_pending ||
        outgoing->reap_queued ||
        outgoing->reap_next != NULL ||
        cpu->reap_count == UINT64_MAX) {

        for (;;) {
            asm volatile ("cli; hlt" ::: "memory");
        }
    }

    /*
     * Account for the final execution interval.
     *
     * Termination must still work if the clocksource
     * unexpectedly fails, so accounting errors do not
     * prevent the thread from exiting.
     */
    uint64_t now_ns = 0;

    bool have_time =
        time_get_monotonic_ns(&now_ns) == TIME_STATUS_OK;

    if (have_time &&
        outgoing->last_start_ns != 0 &&
        now_ns >= outgoing->last_start_ns) {

        uint64_t elapsed_ns =
            now_ns - outgoing->last_start_ns;


        if (elapsed_ns != 0) {

            if (outgoing->scheduling_class == THREAD_CLASS_FAIR) {

                (void)scheduler_fair_account_runtime(
                    outgoing,
                    elapsed_ns
                );

            } else if (outgoing->runtime_ns <=
                       UINT64_MAX - elapsed_ns) {

                outgoing->runtime_ns += elapsed_ns;
            }
        }
    }

    /*
     * Do not select the outgoing thread again.
     * It is about to become permanently terminated.
     */
    struct thread *selected =
        scheduler_run_queue_pick_next(&cpu->run_queue);

    /*
     * If no ordinary thread is runnable, use the idle thread.
     */
    if (selected == NULL) {
        selected = cpu->idle_thread;
    }

    if (selected == NULL ||
        selected == outgoing ||
        selected->state != THREAD_STATE_READY ||
        selected->run_queue_owner != NULL) {

        for (;;) {
            asm volatile ("cli; hlt" ::: "memory");
        }
    }

    /*
    * Activate the selected thread's address space before
    * permanently terminating the outgoing thread.
    */
    scheduler_activate_thread_arch_state_or_halt(cpu, selected);

    /*
    * Record the exit result and permanently terminate
    * the outgoing thread.
    */
    outgoing->exit_code = exit_code;
    outgoing->state = THREAD_STATE_TERMINATED;

    /*
     * Put the terminated thread on the reaper queue.
     *
     * Its stack and TCB remain allocated until another
     * thread safely performs the cleanup.
     */
    outgoing->reap_next = NULL;
    outgoing->reap_pending = true;
    outgoing->reap_queued = true;

    if (cpu->reap_tail != NULL) {
        cpu->reap_tail->reap_next = outgoing;
    } else {
        cpu->reap_head = outgoing;
    }

    cpu->reap_tail = outgoing;
    cpu->reap_count++;

    /*
     * Publish the selected thread as the running thread.
     */
    selected->state = THREAD_STATE_RUNNING;
    selected->last_cpu = cpu->logical_id;
    selected->last_start_ns = have_time ? now_ns : 0;

    cpu->current_thread = selected;

    if (cpu->context_switch_count != UINT64_MAX) {
        cpu->context_switch_count++;
    }

    /*
     * Switch away permanently.
     *
     * This terminated thread must never execute again.
     */
    arch_context_switch(
        &outgoing->context,
        &selected->context
    );

    /*
     * Reaching here means a terminated thread was
     * incorrectly scheduled again. Do not continue.
     */
    for (;;) {
        asm volatile ("cli; hlt" ::: "memory");
    }
}
