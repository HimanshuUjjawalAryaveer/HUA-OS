#include "scheduler/thread.h"
#include "scheduler/scheduler.h"
#include "arch/x86_64/thread_bootstrap.h"
#include "process/process.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "arch/x86_64/irq_flags.h"


/*
 * Thread IDs are monotonically increasing.
 *
 * This is sufficient for the current single-CPU stage.
 * SMP support will require synchronization.
 */
static thread_id_t next_thread_id = 1;


/*
 * Zero memory without relying on hosted libc.
 */
static void thread_zero_memory(void *memory, size_t size) {

    uint8_t *bytes = (uint8_t *)memory;

    for (size_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}


/*
 * Copy a name while guaranteeing null termination.
 */
static void thread_copy_name(
    char destination[THREAD_NAME_MAX],
    const char *source
) {

    if (source == NULL || source[0] == '\0') {
        source = "unnamed";
    }

    size_t i = 0;

    while (i < THREAD_NAME_MAX - 1 &&
           source[i] != '\0') {

        destination[i] = source[i];
        i++;
    }

    destination[i] = '\0';
}


/*
 * Allocate a unique, nonzero thread ID.
 */
static bool thread_allocate_id(thread_id_t *id_out) {

    if (id_out == NULL || next_thread_id == 0) {
        return false;
    }

    *id_out = next_thread_id++;

    return true;
}


/*
 * Initialize common thread scheduling metadata.
 *
 * The caller must have already zeroed the TCB.
 */
static void thread_initialize_metadata(
    struct thread *thread,
    const char *name
) {

    thread_copy_name(thread->name, name);

    thread->scheduling_class = THREAD_CLASS_FAIR;

    thread->realtime_priority = 0;
    thread->realtime_policy = THREAD_RT_ROUND_ROBIN;
    thread->realtime_quantum_ns = 0;

    thread->fair_weight = THREAD_FAIR_WEIGHT_DEFAULT;
    thread->virtual_runtime = 0;
    thread->virtual_deadline = 0;

    thread->fair_slice_ns =
        THREAD_FAIR_DEFAULT_SLICE_NS;

    thread->runtime_ns = 0;
    thread->last_start_ns = 0;
    thread->slice_runtime_ns = 0;

    /*
     * Only logical CPU 0 is currently active.
     */
    thread->affinity_mask = 1ULL;
    thread->last_cpu = 0;

    thread->preempt_count = 0;
    thread->exit_code = 0;
}


/*
 * Calculate a page-aligned stack allocation size.
 */
static bool thread_calculate_stack_size(
    size_t requested_size,
    size_t *aligned_size_out
) {

    if (aligned_size_out == NULL) {
        return false;
    }

    if (requested_size == 0) {
        requested_size = THREAD_STACK_DEFAULT_SIZE;
    }

    if (requested_size < THREAD_STACK_MIN_SIZE) {
        return false;
    }

    /*
     * Reject overflow before rounding up.
     */
    if (requested_size >
        SIZE_MAX - (PMM_PAGE_SIZE - 1ULL)) {

        return false;
    }

    size_t aligned_size =
        (requested_size + PMM_PAGE_SIZE - 1ULL) &
        ~(size_t)(PMM_PAGE_SIZE - 1ULL);

    if (aligned_size < THREAD_STACK_MIN_SIZE) {
        return false;
    }

    *aligned_size_out = aligned_size;

    return true;
}


/*
 * Construct the initial cooperative context for a new thread.
 *
 * arch_context_switch() restores RSP and executes RET.
 *
 * Therefore the initial stack must contain:
 *
 *     [RSP + 0]  arch_thread_bootstrap
 *     [RSP + 8]  arch_thread_return_trap
 *
 * After RET enters the bootstrap, RSP % 16 == 8,
 * as required by the System V AMD64 ABI.
 */
static bool thread_initialize_context(struct thread *thread) {

    if (thread == NULL ||
        thread->stack_base == NULL ||
        thread->stack_size < THREAD_STACK_MIN_SIZE ||
        thread->stack_top == 0) {

        return false;
    }

    if ((thread->stack_top & 15ULL) != 0 ||
        thread->stack_top < (uintptr_t)thread->stack_base) {

        return false;
    }

    if (thread->stack_top -
        (uintptr_t)thread->stack_base != thread->stack_size) {

        return false;
    }

    if (thread->stack_top < 16ULL) {
        return false;
    }

    uintptr_t initial_rsp = thread->stack_top - 16ULL;

    if ((initial_rsp & 15ULL) != 0) {
        return false;
    }

    uintptr_t *initial_stack =
        (uintptr_t *)initial_rsp;

    initial_stack[0] =
        (uintptr_t)arch_thread_bootstrap;

    initial_stack[1] =
        (uintptr_t)arch_thread_return_trap;

    /*
     * R12 is callee-saved by arch_context_switch().
     *
     * The assembly bootstrap uses it to recover the TCB
     * pointer and pass that pointer to the C bootstrap.
     */
    thread->context.r12 = (uint64_t)(uintptr_t)thread;

    thread->context.rsp = (uint64_t)initial_rsp;

    return true;
}


/*
 * Create a new thread.
 *
 * This function allocates memory but does not:
 * - Insert the thread into a run queue.
 * - Mark it runnable.
 * - Perform a context switch.
 */
struct thread *thread_create(
    const char *name,
    thread_entry_t entry,
    void *argument,
    size_t stack_size
) {

    if (entry == NULL) {
        return NULL;
    }

    size_t actual_stack_size = 0;

    if (!thread_calculate_stack_size(
            stack_size,
            &actual_stack_size
        )) {

        return NULL;
    }

    /*
     * Reserve an ID before allocating resources.
     * Failed creation may leave an unused ID.
     */
    thread_id_t id = 0;

    if (!thread_allocate_id(&id)) {
        return NULL;
    }

    /*
     * Allocate the thread control block.
     */
    struct thread *thread =
        (struct thread *)kmalloc(sizeof(struct thread));

    if (thread == NULL) {
        return NULL;
    }

    thread_zero_memory(thread, sizeof(struct thread));

    if (((uintptr_t)thread & 15ULL) != 0) {
        kfree(thread);
        return NULL;
    }

    /*
     * Allocate the kernel stack.
     *
     * The requested allocation is page-aligned in size.
     * The heap's page-backed allocator returns a
     * page-aligned virtual address.
     */
    void *stack = kmalloc(actual_stack_size);

    if (stack == NULL) {
        kfree(thread);
        return NULL;
    }

    uintptr_t stack_base = (uintptr_t)stack;

    if ((stack_base & (PMM_PAGE_SIZE - 1ULL)) != 0 ||
        stack_base > UINTPTR_MAX - actual_stack_size) {

        kfree(stack);
        kfree(thread);
        return NULL;
    }

    uintptr_t stack_top =
        stack_base + actual_stack_size;

    if ((stack_top & 15ULL) != 0) {
        kfree(stack);
        kfree(thread);
        return NULL;
    }

    /*
     * Initialize thread identity and metadata.
     */
    thread->id = id;

    thread_initialize_metadata(thread, name);

    thread->state = THREAD_STATE_NEW;

    thread->stack_base = stack;
    thread->stack_size = actual_stack_size;
    thread->stack_top = stack_top;
    thread->stack_owned = true;

    thread->entry = entry;
    thread->entry_argument = argument;

    /*
     * Construct the initial x86_64 execution context.
     */
    if (!thread_initialize_context(thread)) {

        kfree(stack);
        kfree(thread);
        return NULL;
    }

    return thread;
}


/*
 * Create a thread owned by a process.
 *
 * process_attach_thread() atomically establishes:
 *     - TCB process ownership.
 *     - Borrowed address-space ownership.
 *     - Process thread-list membership.
 *     - Process reference accounting.
 */
struct thread *thread_create_for_process(
    struct process *process,
    const char *name,
    thread_entry_t entry,
    void *argument,
    size_t stack_size
) {

    if (process == NULL) {
        return NULL;
    }

    /*
     * Allocate the ordinary NEW thread first.
     */
    struct thread *thread = thread_create(
        name,
        entry,
        argument,
        stack_size
    );

    if (thread == NULL) {
        return NULL;
    }

    /*
     * Atomically attach the completed TCB to its process.
     */
    if (!process_attach_thread(process, thread)) {

        /*
         * Attachment did not succeed.
         * The TCB must remain process-independent.
         */
        if (!thread_destroy(thread)) {

            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt" ::: "memory");
            }
        }

        return NULL;
    }

    return thread;
}


/*
 * Configure a detached NEW thread for real-time scheduling.
 */
bool thread_set_realtime(
    struct thread *thread,
    uint8_t priority,
    thread_rt_policy_t policy,
    uint64_t quantum_ns
) {

    if (thread == NULL) {
        return false;
    }

    /*
     * Validate the priority before changing any metadata.
     */
    if (priority >= THREAD_RT_PRIORITY_LEVELS) {
        return false;
    }

    /*
     * Only the two supported real-time policies are valid.
     */
    if (policy != THREAD_RT_FIFO &&
        policy != THREAD_RT_ROUND_ROBIN) {

        return false;
    }

    /*
     * FIFO has no automatic time quantum.
     */
    if (policy == THREAD_RT_FIFO) {

        if (quantum_ns != 0) {
            return false;
        }
    }

    /*
     * A zero RR quantum selects HUA's default.
     */
    else {

        if (quantum_ns == 0) {
            quantum_ns = THREAD_RT_DEFAULT_QUANTUM_NS;
        }

        if (quantum_ns < THREAD_RT_QUANTUM_MIN_NS ||
            quantum_ns > THREAD_RT_QUANTUM_MAX_NS) {

            return false;
        }
    }

    /*
     * RT configuration is allowed only while the thread
     * is NEW and completely detached from scheduler queues.
     */
    if (thread->state != THREAD_STATE_NEW ||
        thread->run_previous != NULL ||
        thread->run_next != NULL ||
        thread->run_queue_owner != NULL ||
        thread->wait_previous != NULL ||
        thread->wait_next != NULL ||
        thread->wait_channel != NULL ||
        thread->reap_next != NULL ||
        thread->reap_pending ||
        thread->reap_queued) {

        return false;
    }

    /*
     * Commit the configuration only after all validation
     * has succeeded.
     */
    thread->scheduling_class = THREAD_CLASS_REALTIME;
    thread->realtime_priority = priority;
    thread->realtime_policy = policy;
    thread->realtime_quantum_ns = quantum_ns;

    /*
     * Start with a fresh RR time slice.
     */
    thread->slice_runtime_ns = 0;

    return true;
}


/*
 * Adopt the current kernel execution as the bootstrap thread.
 *
 * The current Limine-provided kmain stack is borrowed.
 * We intentionally do not invent its base, size, or top.
 *
 * The context is saved automatically the first time
 * arch_context_switch() switches away from this thread.
 */
struct thread *thread_adopt_bootstrap(const char *name) {

    thread_id_t id = 0;

    if (!thread_allocate_id(&id)) {
        return NULL;
    }

    struct thread *thread =
        (struct thread *)kmalloc(sizeof(struct thread));

    if (thread == NULL) {
        return NULL;
    }

    thread_zero_memory(thread, sizeof(struct thread));

    if (((uintptr_t)thread & 15ULL) != 0) {
        kfree(thread);
        return NULL;
    }

    thread->id = id;

    thread_initialize_metadata(thread, name);

    thread->state = THREAD_STATE_RUNNING;

    /*
     * No owned stack allocation exists for this thread.
     */
    thread->stack_base = NULL;
    thread->stack_size = 0;
    thread->stack_top = 0;
    thread->stack_owned = false;

    /*
     * The currently executing kmain context is not a
     * newly created entry function.
     */
    thread->entry = NULL;
    thread->entry_argument = NULL;

    return thread;
}


/*
 * Entry point called by the assembly bootstrap trampoline.
 *
 * This function must never return.
 */
__attribute__((noreturn))
void thread_bootstrap(struct thread *thread) {

    if (thread == NULL ||
        thread->state != THREAD_STATE_RUNNING ||
        thread->entry == NULL) {

        goto invalid_thread;
    }

    /*
     * Reclaim any threads terminated before this
     * newly created thread began executing.
     *
     * We are executing on this thread's own stack,
     * so reclaiming other threads is safe.
     */
    scheduler_reap_terminated_threads();

    /*
     * The scheduler switches to new threads with
     * interrupts disabled. Enable them only after
     * the thread's execution state is established.
     */
    asm volatile ("sti" ::: "memory");

    /*
     * Execute the thread's entry function.
     */
    thread->entry(thread->entry_argument);

    /*
     * A returned entry function means the thread
     * has completed. Never return through the
     * assembly bootstrap trampoline.
     */
    thread_exit(0);

invalid_thread:

    asm volatile ("cli" ::: "memory");

    for (;;) {
        asm volatile ("hlt" ::: "memory");
    }
}


/*
 * Destroy a thread that has never become runnable.
 */
bool thread_destroy(struct thread *thread) {

    if (thread == NULL) {
        return false;
    }

    bool is_new_thread =
        thread->state == THREAD_STATE_NEW &&
        !thread->reap_pending &&
        !thread->reap_queued &&
        thread->reap_next == NULL;

    bool is_reaped_thread =
        thread->state == THREAD_STATE_TERMINATED &&
        thread->reap_pending &&
        !thread->reap_queued &&
        thread->reap_next == NULL;

    if (!is_new_thread && !is_reaped_thread) {
        return false;
    }

    /*
    * Process ownership and the cached address space must
    * either both exist or both be absent.
    *
    * Reject inconsistent TCB ownership instead of freeing
    * memory while a borrowed pointer may still be in use.
    */
    if ((thread->process == NULL) !=
        (thread->address_space == NULL)) {

        return false;
    }

    /*
     * A process-owned thread that never started running
     * must release its process attachment before destruction.
     *
     * A terminated thread must already have released its
     * process attachment through the scheduler reaper.
     */
    if (thread->process != NULL) {

        if (!is_new_thread ||
            thread->address_space == NULL) {

            return false;
        }

        if (!process_detach_thread(
                thread->process,
                thread
            )) {

            return false;
        }

        /*
        * The process attachment has been released.
        * Neither borrowed pointer may remain in the TCB.
        */
        thread->process = NULL;
        thread->address_space = NULL;
    }

    /*
    * A detached TCB must not retain process-list linkage.
    */
    if (thread->process != NULL ||
        thread->address_space != NULL ||
        thread->process_previous != NULL ||
        thread->process_next != NULL) {

        return false;
    }

    /*
     * A NEW thread must not be linked into queues.
     */
    if (thread->run_queue_owner != NULL ||
        thread->run_previous != NULL ||
        thread->run_next != NULL ||
        thread->wait_previous != NULL ||
        thread->wait_next != NULL ||
        thread->wait_channel != NULL ||
        thread->sleep_queued ||
        thread->sleep_previous != NULL ||
        thread->sleep_next != NULL ||
        thread->sleep_deadline_ns != 0) {

        return false;
    }

    if (is_reaped_thread) {
        thread->reap_pending = false;
    }

    /*
     * Release only stacks owned by this thread.
     */
    if (thread->stack_owned) {

        if (thread->stack_base == NULL) {
            return false;
        }

        kfree(thread->stack_base);
    }

    /*
     * Release the TCB last.
     */
    kfree(thread);

    return true;
}

/*
 * Disable involuntary preemption of the current thread.
 *
 * The counter supports nested critical sections.
 *
 * Interrupt state is preserved while modifying the counter
 * so a timer interrupt cannot observe a partially updated
 * preemption state.
 *
 * Current implementation: CPU0 only.
 */
bool thread_preempt_disable(void) {

    uint64_t flags = arch_irq_save_disable();

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        cpu->current_thread == NULL ||
        cpu->current_thread->state != THREAD_STATE_RUNNING ||
        cpu->current_thread->preempt_count == UINT32_MAX) {

        arch_irq_restore(flags);
        return false;
    }

    cpu->current_thread->preempt_count++;

    arch_irq_restore(flags);

    return true;
}


/*
 * Release one level of preemption protection.
 *
 * This function does not perform a context switch.
 *
 * If a timer interrupt requested rescheduling while
 * preemption was disabled, the pending request remains
 * recorded in scheduler_cpu.reschedule_requested.
 *
 * The next timer scheduling boundary reevaluates the
 * current scheduling policy and performs a switch if
 * preemption is still required.
 *
 * This avoids bypassing:
 * - Real-time FIFO semantics.
 * - Real-time round-robin quantum handling.
 * - Fair EEVDF selection.
 *
 * Current implementation: CPU0 only.
 */
bool thread_preempt_enable(void) {

    uint64_t flags = arch_irq_save_disable();

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        cpu->current_thread == NULL ||
        cpu->current_thread->state != THREAD_STATE_RUNNING ||
        cpu->current_thread->preempt_count == 0) {

        arch_irq_restore(flags);
        return false;
    }

    cpu->current_thread->preempt_count--;

    arch_irq_restore(flags);

    return true;
}
