#include "tests/scheduler_integration_tests.h"

#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "scheduler/wait_queue.h"
#include "process/process.h"
#include "mm/vmm.h"
#include "arch/x86_64/irq_flags.h"
#include "arch/x86_64/gdt.h"


/*
 * The worker writes this flag immediately before returning.
 *
 * Volatile ensures the compiler does not optimize away
 * the observable communication between the two threads.
 */
static volatile bool worker_executed;

static volatile bool worker_tss_stack_valid;

/*
 * Ordinary kernel-thread worker.
 *
 * Verify that the scheduler installed this thread's
 * own kernel stack in TSS.RSP0 before execution began.
 */
static void scheduler_integration_worker(void *argument) {

    (void)argument;

    struct thread *current =
        scheduler_get_current_thread(0);

    uintptr_t observed_tss_stack = 0;

    worker_tss_stack_valid =
        current != NULL &&
        current->stack_owned &&
        current->stack_base != NULL &&
        current->stack_top != 0 &&
        gdt_get_kernel_stack(&observed_tss_stack) &&
        observed_tss_stack == current->stack_top;

    worker_executed = true;
}

/*
 * Real blocking/wakeup integration test state.
 *
 * The queue is static because the worker may remain
 * blocked after its original function invocation has
 * switched away from its stack.
 */
static struct wait_queue blocking_test_queue;

static volatile bool blocking_worker_entered;
static volatile bool blocking_worker_resumed;
static volatile bool blocking_worker_block_failed;

#define SCHEDULER_CR3_TEST_WORKER_COUNT 3U

typedef struct {

    process_t *process;

    struct thread *expected_thread;

    vmm_address_space_t *expected_space;
    vmm_address_space_t *other_space;

    uint64_t expected_root;

    uint32_t slot;
    uint32_t expected_order;

} scheduler_cr3_test_context_t;


/*
 * Worker contexts:
 *
 * 0 = Process A / thread 1
 * 1 = Process B / thread 1
 * 2 = Process A / thread 2
 */
static scheduler_cr3_test_context_t
    scheduler_cr3_test_contexts[
        SCHEDULER_CR3_TEST_WORKER_COUNT
    ];


/*
 * Results written by the actual executing workers.
 *
 * Atomic access prevents compiler reordering and makes
 * these fields suitable for later SMP test expansion.
 */
static uint32_t scheduler_cr3_test_sequence = 0;

static uint32_t scheduler_cr3_test_order[
    SCHEDULER_CR3_TEST_WORKER_COUNT
];

static uint64_t scheduler_cr3_test_observed_roots[
    SCHEDULER_CR3_TEST_WORKER_COUNT
];

static bool scheduler_cr3_test_results[
    SCHEDULER_CR3_TEST_WORKER_COUNT
];

/*
 * Execute under a process-specific address space and
 * inspect the actual hardware CR3 and VMM bookkeeping.
 *
 * This runs inside a real scheduled thread, not from
 * the bootstrap test context.
 */
static void scheduler_cr3_test_worker(void *argument) {

    scheduler_cr3_test_context_t *context =
        (scheduler_cr3_test_context_t *)argument;

    if (context == NULL ||
        context->slot >= SCHEDULER_CR3_TEST_WORKER_COUNT ||
        context->process == NULL ||
        context->expected_thread == NULL ||
        context->expected_space == NULL ||
        context->other_space == NULL) {

        return;
    }

    /*
     * Record the actual execution order.
     */
    uint32_t observed_order =
        __atomic_add_fetch(
            &scheduler_cr3_test_sequence,
            1,
            __ATOMIC_SEQ_CST
        );

    /*
     * Inspect the currently executing thread.
     */
    struct thread *current =
        scheduler_get_current_thread(0);

    bool thread_identity_ok =
        current == context->expected_thread &&
        current != NULL &&
        current->state == THREAD_STATE_RUNNING &&
        current->process == context->process &&
        current->address_space == context->expected_space;

    /*
    * Validate this process-owned thread's kernel stack.
    */
    uintptr_t observed_tss_stack = 0;

    bool stack_metadata_ok = false;
    bool tss_stack_ok = false;

    if (current != NULL &&
        current->stack_owned &&
        current->stack_base != NULL &&
        current->stack_size >= THREAD_STACK_MIN_SIZE &&
        current->stack_top != 0) {

        uintptr_t stack_base =
            (uintptr_t)current->stack_base;

        if (current->stack_size <=
            UINTPTR_MAX - stack_base) {

            uintptr_t expected_stack_top =
                stack_base + current->stack_size;

            stack_metadata_ok =
                expected_stack_top == current->stack_top &&
                (expected_stack_top & 0xFULL) == 0;

            tss_stack_ok =
                stack_metadata_ok &&
                gdt_get_kernel_stack(&observed_tss_stack) &&
                observed_tss_stack == expected_stack_top;
        }
    }

    /*
     * Inspect the hardware CR3 and VMM's active-space
     * bookkeeping.
     */
    vmm_address_space_t *active_space = NULL;

    uint64_t active_root = 0;

    bool active_query_ok =
        vmm_test_get_active_address_space(
            0,
            &active_space,
            &active_root
        );

    /*
     * Inspect the active CPU masks.
     */
    uint64_t expected_mask = 0;
    uint64_t other_mask = UINT64_MAX;
    uint64_t kernel_mask = UINT64_MAX;

    bool masks_query_ok =
        vmm_test_get_address_space_active_cpu_mask(
            context->expected_space,
            &expected_mask
        ) &&
        vmm_test_get_address_space_active_cpu_mask(
            context->other_space,
            &other_mask
        ) &&
        vmm_test_get_address_space_active_cpu_mask(
            vmm_kernel_address_space(),
            &kernel_mask
        );

    /*
     * Attempt to destroy a process that still has
     * attached threads.
     *
     * This must always be rejected.
     */
    bool live_process_destruction_rejected =
        !process_destroy(context->process);

    bool passed =
        thread_identity_ok &&
        stack_metadata_ok &&
        tss_stack_ok &&
        active_query_ok &&
        active_space == context->expected_space &&
        active_root == context->expected_root &&
        masks_query_ok &&
        expected_mask == 1ULL &&
        other_mask == 0 &&
        kernel_mask == 0 &&
        live_process_destruction_rejected &&
        observed_order == context->expected_order;

    /*
     * Publish observations for the bootstrap test.
     */
    __atomic_store_n(
        &scheduler_cr3_test_order[context->slot],
        observed_order,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &scheduler_cr3_test_observed_roots[context->slot],
        active_root,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &scheduler_cr3_test_results[context->slot],
        passed,
        __ATOMIC_RELEASE
    );
}

/*
 * This worker enters a real wait queue and cannot
 * continue until another thread explicitly wakes it.
 */
static void scheduler_blocking_worker(void *argument) {

    struct wait_queue *queue =
        (struct wait_queue *)argument;

    blocking_worker_entered = true;

    /*
     * This call changes the worker to BLOCKED, queues it,
     * and switches to another runnable thread.
     *
     * It returns only after the worker is woken and
     * selected by the scheduler again.
     */
    if (!thread_block(queue)) {

        blocking_worker_block_failed = true;
        return;
    }

    /*
     * Reaching this line proves that the blocked
     * thread resumed after its wakeup.
     */
    blocking_worker_resumed = true;
}


/*
 * Test the complete real blocking/wakeup lifecycle:
 *
 * 1. Create and enqueue a worker.
 * 2. Let it execute and block on a real wait queue.
 * 3. Verify that it is actually BLOCKED.
 * 4. Wake it using thread_wake().
 * 5. Verify that a duplicate wake is rejected.
 * 6. Let it resume after thread_block().
 * 7. Let it terminate and be reclaimed.
 */
static bool scheduler_integration_test_blocking(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap
) {

    if (cpu == NULL ||
        bootstrap == NULL ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        bootstrap->preempt_count != 0) {

        return false;
    }

    /*
     * Start from a clean scheduler state.
     */
    if (cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    if (!wait_queue_init(&blocking_test_queue)) {
        return false;
    }

    if (!wait_queue_is_empty(&blocking_test_queue) ||
        !wait_queue_validate(&blocking_test_queue)) {

        return false;
    }

    /*
     * Preserve scheduler diagnostics.
     *
     * Expected switches:
     *
     *   bootstrap -> worker       (yield)
     *   worker    -> bootstrap    (block)
     *   bootstrap -> worker       (yield)
     *   worker    -> bootstrap    (exit)
     */
    uint64_t initial_context_switches =
        cpu->context_switch_count;

    uint64_t initial_voluntary_yields =
        cpu->voluntary_yield_count;

    if (initial_context_switches > UINT64_MAX - 4 ||
        initial_voluntary_yields > UINT64_MAX - 2) {

        return false;
    }

    blocking_worker_entered = false;
    blocking_worker_resumed = false;
    blocking_worker_block_failed = false;

    /*
     * Create the worker.
     */
    struct thread *worker = thread_create(
        "blocking-test",
        scheduler_blocking_worker,
        &blocking_test_queue,
        0
    );

    if (worker == NULL) {
        return false;
    }

    /*
     * Make the worker runnable.
     */
    if (!scheduler_enqueue_thread(0, worker)) {

        (void)thread_destroy(worker);
        return false;
    }

    /*
     * The bootstrap thread yields to the worker.
     *
     * The worker enters thread_block(). Because bootstrap
     * is now runnable, the scheduler switches back here.
     */
    if (!thread_yield()) {
        return false;
    }

    /*
     * The worker must have reached the blocking operation.
     */
    if (!blocking_worker_entered ||
        blocking_worker_block_failed ||
        blocking_worker_resumed) {

        return false;
    }

    /*
     * The worker must really be waiting on this queue.
     *
     * It is still alive, so inspecting its state here
     * is safe.
     */
    if (worker->state != THREAD_STATE_BLOCKED ||
        worker->wait_channel != &blocking_test_queue ||
        wait_queue_count(&blocking_test_queue) != 1 ||
        !wait_queue_validate(&blocking_test_queue)) {

        return false;
    }

    /*
     * Wake the specific blocked worker.
     *
     * This removes it from the wait queue, changes it
     * to READY, and inserts it into CPU0's run queue.
     */
    if (!thread_wake(worker)) {
        return false;
    }

    /*
     * The worker must no longer belong to the wait queue.
     */
    if (!wait_queue_is_empty(&blocking_test_queue) ||
        worker->state != THREAD_STATE_READY ||
        worker->wait_channel != NULL ||
        !wait_queue_validate(&blocking_test_queue)) {

        return false;
    }

    /*
     * A second wake must fail: the worker is READY,
     * not BLOCKED, and has no wait-channel membership.
     */
    if (thread_wake(worker)) {
        return false;
    }

    /*
     * The worker is now runnable. Yield so it can resume
     * after thread_block(), set its completion flag,
     * return, and terminate.
     */
    if (!thread_yield()) {
        return false;
    }

    /*
     * Do not inspect worker here.
     *
     * Its TCB and stack may already have been reclaimed.
     * Use only independent flags and scheduler state.
     */
    if (!blocking_worker_resumed ||
        blocking_worker_block_failed) {

        return false;
    }

    if (cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        return false;
    }

    /*
     * The wait queue and scheduler queues must be clean.
     */
    if (!wait_queue_is_empty(&blocking_test_queue) ||
        !wait_queue_validate(&blocking_test_queue) ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    /*
     * Four context switches and two voluntary yields
     * must have occurred.
     */
    if (cpu->context_switch_count !=
        initial_context_switches + 4) {

        return false;
    }

    if (cpu->voluntary_yield_count !=
        initial_voluntary_yields + 2) {

        return false;
    }

    return true;
}

/*
 * Move a process toward termination during test cleanup.
 *
 * A process with no threads may still be NEW.
 * A process with attached threads must first enter EXITING.
 */
static bool scheduler_cr3_test_begin_exit_if_needed(
    process_t *process
) {

    if (process == NULL) {
        return true;
    }

    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = 0;

    if (!process_get_info(
            process,
            NULL,
            &state,
            &thread_count,
            NULL,
            0
        )) {

        return false;
    }

    if (state == PROCESS_STATE_RUNNING) {
        return process_begin_exit(process);
    }

    if (state == PROCESS_STATE_NEW) {
        return thread_count == 0;
    }

    return state == PROCESS_STATE_EXITING ||
           state == PROCESS_STATE_ZOMBIE;
}


/*
 * Discard process-owned worker threads that have not run.
 *
 * An enqueued worker is first removed from the run queue.
 * Its READY state is then returned to NEW while interrupts
 * are disabled, making thread_destroy() legal.
 */
static bool scheduler_cr3_test_discard_unstarted_workers(
    struct scheduler_cpu *cpu,
    struct thread *workers[SCHEDULER_CR3_TEST_WORKER_COUNT]
) {

    if (cpu == NULL || workers == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    bool result = true;

    for (uint32_t i = 0;
         i < SCHEDULER_CR3_TEST_WORKER_COUNT;
         i++) {

        struct thread *thread = workers[i];

        if (thread == NULL) {
            continue;
        }

        /*
         * Remove a partially enqueued worker.
         */
        if (thread->run_queue_owner != NULL) {

            if (thread->run_queue_owner != &cpu->run_queue ||
                !scheduler_dequeue_thread(
                    cpu->logical_id,
                    thread
                )) {

                result = false;
                continue;
            }
        }

        /*
         * A READY thread can be returned to NEW only after
         * all run-queue links have been removed.
         */
        if (thread->state == THREAD_STATE_READY) {

            if (thread->run_queue_owner != NULL ||
                thread->run_previous != NULL ||
                thread->run_next != NULL) {

                result = false;
                continue;
            }

            thread->state = THREAD_STATE_NEW;
        }

        if (thread->state != THREAD_STATE_NEW) {
            result = false;
            continue;
        }

        /*
         * Releases the thread's process attachment and
         * its kernel stack/TCB.
         */
        if (!thread_destroy(thread)) {

            result = false;
            continue;
        }

        workers[i] = NULL;
    }

    arch_irq_restore(flags);

    return result;
}


/*
 * Remove a process from the registry and release the
 * caller's reference.
 *
 * This is only legal after all process-owned threads
 * have released their references.
 */
static bool scheduler_cr3_test_destroy_process(
    process_t **process_pointer
) {

    if (process_pointer == NULL ||
        *process_pointer == NULL) {

        return true;
    }

    process_t *process = *process_pointer;

    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = 0;

    if (!process_get_info(
            process,
            NULL,
            &state,
            &thread_count,
            NULL,
            0
        )) {

        return false;
    }

    if (thread_count != 0) {
        return false;
    }

    /*
     * A RUNNING process with no remaining threads must
     * transition to ZOMBIE before destruction.
     */
    if (state == PROCESS_STATE_RUNNING) {

        if (!process_begin_exit(process)) {
            return false;
        }

        if (!process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            )) {

            return false;
        }
    }

    if (thread_count != 0 ||
        (state != PROCESS_STATE_NEW &&
         state != PROCESS_STATE_ZOMBIE)) {

        return false;
    }

    if (!process_destroy(process)) {
        return false;
    }

    /*
     * Drops the caller's reference.
     *
     * The process releases its owned address space here.
     */
    process_put(process);

    *process_pointer = NULL;

    return true;
}

/*
 * Verify real scheduler-driven address-space transitions.
 *
 * Expected execution order:
 *
 *   Bootstrap       -> Kernel address space
 *   Process A / T1  -> Address space A
 *   Process B / T1  -> Address space B
 *   Process A / T2  -> Address space A
 *   Bootstrap       -> Kernel address space
 *
 * All workers use the same RT FIFO priority so their
 * execution order is deterministic.
 */
static bool scheduler_integration_test_address_space_switching(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap
) {

    if (cpu == NULL ||
        bootstrap == NULL ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        bootstrap->preempt_count != 0 ||
        cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    if (cpu->context_switch_count > UINT64_MAX - 4 ||
        cpu->voluntary_yield_count == UINT64_MAX) {

        return false;
    }

    uint64_t initial_context_switches =
        cpu->context_switch_count;

    uint64_t initial_voluntary_yields =
        cpu->voluntary_yield_count;

    /*
     * Verify the bootstrap thread starts in the permanent
     * kernel address space.
     */
    vmm_address_space_t *kernel_space =
        vmm_kernel_address_space();

    vmm_address_space_t *initial_active_space = NULL;

    uint64_t kernel_root = 0;
    uint64_t initial_active_root = 0;

    uint64_t kernel_initial_mask = 0;
    uintptr_t initial_tss_stack = 0;

    if (kernel_space == NULL ||
        !vmm_address_space_get_root_physical(
            kernel_space,
            &kernel_root
        ) ||
        !gdt_get_kernel_stack(&initial_tss_stack) ||
        initial_tss_stack == 0 ||
        (initial_tss_stack & 0xFULL) != 0 ||
        !vmm_test_get_active_address_space(
            0,
            &initial_active_space,
            &initial_active_root
        ) ||
        !vmm_test_get_address_space_active_cpu_mask(
            kernel_space,
            &kernel_initial_mask
        ) ||
        initial_active_space != kernel_space ||
        initial_active_root != kernel_root ||
        kernel_initial_mask != 1ULL) {

        return false;
    }

    process_t *process_a = NULL;
    process_t *process_b = NULL;

    vmm_address_space_t *space_a = NULL;
    vmm_address_space_t *space_b = NULL;

    uint64_t root_a = 0;
    uint64_t root_b = 0;

    struct thread *workers[
        SCHEDULER_CR3_TEST_WORKER_COUNT
    ] = {
        NULL,
        NULL,
        NULL
    };

    bool workers_dispatched = false;
    bool passed = false;
    bool cleanup_ok = true;

    /*
     * Create two independent user processes.
     *
     * These are still kernel-mode test processes.
     * No ELF image or ring-3 execution is involved.
     */
    process_a = process_create_user("cr3-test-A");

    if (process_a == NULL) {
        goto address_space_test_cleanup;
    }

    process_b = process_create_user("cr3-test-B");

    if (process_b == NULL) {
        goto address_space_test_cleanup;
    }

    /*
     * Obtain borrowed address-space pointers while the
     * test owns references to both processes.
     */
    if (!process_get_address_space(
            process_a,
            &space_a
        ) ||
        !process_get_address_space(
            process_b,
            &space_b
        ) ||
        space_a == NULL ||
        space_b == NULL ||
        space_a == space_b) {

        goto address_space_test_cleanup;
    }

    /*
     * Verify that the two processes have distinct,
     * aligned physical PML4 roots.
     */
    if (!vmm_address_space_get_root_physical(
            space_a,
            &root_a
        ) ||
        !vmm_address_space_get_root_physical(
            space_b,
            &root_b
        ) ||
        root_a == 0 ||
        root_b == 0 ||
        root_a == root_b ||
        (root_a & 0xFFFULL) != 0 ||
        (root_b & 0xFFFULL) != 0) {

        goto address_space_test_cleanup;
    }

    /*
     * Neither process should initially be active.
     */
    uint64_t mask_a = UINT64_MAX;
    uint64_t mask_b = UINT64_MAX;

    if (!vmm_test_get_address_space_active_cpu_mask(
            space_a,
            &mask_a
        ) ||
        !vmm_test_get_address_space_active_cpu_mask(
            space_b,
            &mask_b
        ) ||
        mask_a != 0 ||
        mask_b != 0) {

        goto address_space_test_cleanup;
    }

    /*
     * Reset the shared worker observations.
     */
    __atomic_store_n(
        &scheduler_cr3_test_sequence,
        0,
        __ATOMIC_RELEASE
    );

    for (uint32_t i = 0;
         i < SCHEDULER_CR3_TEST_WORKER_COUNT;
         i++) {

        __atomic_store_n(
            &scheduler_cr3_test_order[i],
            0,
            __ATOMIC_RELEASE
        );

        __atomic_store_n(
            &scheduler_cr3_test_observed_roots[i],
            0,
            __ATOMIC_RELEASE
        );

        __atomic_store_n(
            &scheduler_cr3_test_results[i],
            false,
            __ATOMIC_RELEASE
        );

        scheduler_cr3_test_contexts[i] =
            (scheduler_cr3_test_context_t){0};
    }

    /*
     * Create the workers in the exact order in which
     * they must execute.
     *
     * A1 and A2 belong to the same process and therefore
     * must share the same address space.
     */
    workers[0] = thread_create_for_process(
        process_a,
        "cr3-A-thread-1",
        scheduler_cr3_test_worker,
        &scheduler_cr3_test_contexts[0],
        THREAD_STACK_DEFAULT_SIZE
    );

    if (workers[0] == NULL) {
        goto address_space_test_cleanup;
    }

    workers[1] = thread_create_for_process(
        process_b,
        "cr3-B-thread-1",
        scheduler_cr3_test_worker,
        &scheduler_cr3_test_contexts[1],
        THREAD_STACK_DEFAULT_SIZE
    );

    if (workers[1] == NULL) {
        goto address_space_test_cleanup;
    }

    workers[2] = thread_create_for_process(
        process_a,
        "cr3-A-thread-2",
        scheduler_cr3_test_worker,
        &scheduler_cr3_test_contexts[2],
        THREAD_STACK_DEFAULT_SIZE
    );

    if (workers[2] == NULL) {
        goto address_space_test_cleanup;
    }

    /*
     * Configure every worker with the same real-time FIFO
     * priority.
     *
     * FIFO does not rotate automatically, so each worker
     * exits before the next worker is dispatched.
     */
    for (uint32_t i = 0;
         i < SCHEDULER_CR3_TEST_WORKER_COUNT;
         i++) {

        if (!thread_set_realtime(
                workers[i],
                16,
                THREAD_RT_FIFO,
                0
            )) {

            goto address_space_test_cleanup;
        }
    }

    /*
     * Publish the expected ownership and execution order.
     */
    scheduler_cr3_test_contexts[0] = (
        scheduler_cr3_test_context_t
    ) {
        .process = process_a,
        .expected_thread = workers[0],
        .expected_space = space_a,
        .other_space = space_b,
        .expected_root = root_a,
        .slot = 0,
        .expected_order = 1
    };

    scheduler_cr3_test_contexts[1] = (
        scheduler_cr3_test_context_t
    ) {
        .process = process_b,
        .expected_thread = workers[1],
        .expected_space = space_b,
        .other_space = space_a,
        .expected_root = root_b,
        .slot = 1,
        .expected_order = 2
    };

    scheduler_cr3_test_contexts[2] = (
        scheduler_cr3_test_context_t
    ) {
        .process = process_a,
        .expected_thread = workers[2],
        .expected_space = space_a,
        .other_space = space_b,
        .expected_root = root_a,
        .slot = 2,
        .expected_order = 3
    };

    /*
     * Verify process ownership before execution:
     *
     * Process A: two attached threads.
     * Process B: one attached thread.
     */
    process_state_t state_a = PROCESS_STATE_NEW;
    process_state_t state_b = PROCESS_STATE_NEW;

    uint32_t threads_a = 0;
    uint32_t threads_b = 0;

    if (!process_get_info(
            process_a,
            NULL,
            &state_a,
            &threads_a,
            NULL,
            0
        ) ||
        !process_get_info(
            process_b,
            NULL,
            &state_b,
            &threads_b,
            NULL,
            0
        ) ||
        state_a != PROCESS_STATE_RUNNING ||
        state_b != PROCESS_STATE_RUNNING ||
        threads_a != 2 ||
        threads_b != 1) {

        goto address_space_test_cleanup;
    }

    /*
     * Enqueue in FIFO order:
     *
     * A1 -> B1 -> A2
     */
    for (uint32_t i = 0;
         i < SCHEDULER_CR3_TEST_WORKER_COUNT;
         i++) {

        if (!scheduler_enqueue_thread(0, workers[i])) {
            goto address_space_test_cleanup;
        }
    }

    /*
     * The bootstrap thread yields once.
     *
     * The three workers execute sequentially. Each worker
     * exits normally, and the scheduler switches to the
     * next worker. Finally, execution returns here.
     */
    if (!thread_yield()) {
        goto address_space_test_cleanup;
    }

    /*
     * All worker TCBs have now been reclaimed by the
     * deferred reaper.
     *
     * Never inspect their pointers again.
     */
    workers_dispatched = true;

    for (uint32_t i = 0;
         i < SCHEDULER_CR3_TEST_WORKER_COUNT;
         i++) {

        workers[i] = NULL;
    }

    /*
     * Verify all three workers actually ran in the
     * expected order and observed their expected roots.
     */
    bool worker_results_ok = true;

    for (uint32_t i = 0;
         i < SCHEDULER_CR3_TEST_WORKER_COUNT;
         i++) {

        uint32_t observed_order =
            __atomic_load_n(
                &scheduler_cr3_test_order[i],
                __ATOMIC_ACQUIRE
            );

        uint64_t observed_root =
            __atomic_load_n(
                &scheduler_cr3_test_observed_roots[i],
                __ATOMIC_ACQUIRE
            );

        bool worker_passed =
            __atomic_load_n(
                &scheduler_cr3_test_results[i],
                __ATOMIC_ACQUIRE
            );

        if (!worker_passed ||
            observed_order != i + 1 ||
            observed_root !=
                scheduler_cr3_test_contexts[i].expected_root) {

            worker_results_ok = false;
        }
    }

    uint32_t total_workers_executed =
        __atomic_load_n(
            &scheduler_cr3_test_sequence,
            __ATOMIC_ACQUIRE
        );

    /*
     * All process-owned threads must have exited and
     * been reaped.
     */
    state_a = PROCESS_STATE_NEW;
    state_b = PROCESS_STATE_NEW;

    threads_a = UINT32_MAX;
    threads_b = UINT32_MAX;

    bool process_lifecycle_ok =
        process_get_info(
            process_a,
            NULL,
            &state_a,
            &threads_a,
            NULL,
            0
        ) &&
        process_get_info(
            process_b,
            NULL,
            &state_b,
            &threads_b,
            NULL,
            0
        ) &&
        state_a == PROCESS_STATE_ZOMBIE &&
        state_b == PROCESS_STATE_ZOMBIE &&
        threads_a == 0 &&
        threads_b == 0;

    /*
     * The bootstrap thread must have its kernel address
     * space restored after all three transitions.
     */
    vmm_address_space_t *final_active_space = NULL;
    uint64_t final_active_root = 0;
    uintptr_t final_tss_stack = 0;

    uint64_t final_kernel_mask = 0;
    uint64_t final_mask_a = UINT64_MAX;
    uint64_t final_mask_b = UINT64_MAX;

    bool final_activation_ok =
        vmm_test_get_active_address_space(
            0,
            &final_active_space,
            &final_active_root
        ) &&
        gdt_get_kernel_stack(&final_tss_stack) &&
        final_tss_stack == initial_tss_stack &&
        vmm_test_get_address_space_active_cpu_mask(
            kernel_space,
            &final_kernel_mask
        ) &&
        vmm_test_get_address_space_active_cpu_mask(
            space_a,
            &final_mask_a
        ) &&
        vmm_test_get_address_space_active_cpu_mask(
            space_b,
            &final_mask_b
        ) &&
        final_active_space == kernel_space &&
        final_active_root == kernel_root &&
        final_kernel_mask == 1ULL &&
        final_mask_a == 0 &&
        final_mask_b == 0;

    /*
     * The exact expected scheduling path contains
     * four context switches and one voluntary yield.
     */
    bool scheduling_counts_ok =
        cpu->context_switch_count ==
            initial_context_switches + 4 &&
        cpu->voluntary_yield_count ==
            initial_voluntary_yields + 1;

    /*
     * All scheduler queues must be empty.
     */
    bool scheduler_cleanup_ok =
        cpu->current_thread == bootstrap &&
        bootstrap->state == THREAD_STATE_RUNNING &&
        cpu->run_queue.runnable_count == 0 &&
        cpu->reap_count == 0 &&
        cpu->reap_head == NULL &&
        cpu->reap_tail == NULL;

    passed =
        worker_results_ok &&
        total_workers_executed ==
            SCHEDULER_CR3_TEST_WORKER_COUNT &&
        process_lifecycle_ok &&
        final_activation_ok &&
        scheduling_counts_ok &&
        scheduler_cleanup_ok;

address_space_test_cleanup:

    /*
     * If dispatch never completed, no worker should have
     * executed. Put processes into exit and safely discard
     * all NEW or partially enqueued worker threads.
     */
    if (!workers_dispatched) {

        if (!scheduler_cr3_test_begin_exit_if_needed(process_a)) {
            cleanup_ok = false;
        }

        if (!scheduler_cr3_test_begin_exit_if_needed(process_b)) {
            cleanup_ok = false;
        }

        if (!scheduler_cr3_test_discard_unstarted_workers(
                cpu,
                workers
            )) {

            cleanup_ok = false;
        }

    } else {

        /*
         * Their TCBs were already reclaimed.
         */
        for (uint32_t i = 0;
             i < SCHEDULER_CR3_TEST_WORKER_COUNT;
             i++) {

            workers[i] = NULL;
        }
    }

    /*
     * Recover address-space references if an earlier
     * setup failure happened before they were retrieved.
     */
    if (process_a != NULL && space_a == NULL) {
        (void)process_get_address_space(process_a, &space_a);
    }

    if (process_b != NULL && space_b == NULL) {
        (void)process_get_address_space(process_b, &space_b);
    }

    /*
     * Ensure that cleanup itself never frees an address
     * space that is still active in CR3.
     */
    vmm_address_space_t *cleanup_active_space = NULL;
    uint64_t cleanup_active_root = 0;

    bool kernel_restored =
        vmm_test_get_active_address_space(
            0,
            &cleanup_active_space,
            &cleanup_active_root
        ) &&
        cleanup_active_space == kernel_space &&
        cleanup_active_root == kernel_root;

    if (!kernel_restored) {

        if (vmm_address_space_activate(
                0,
                kernel_space
            )) {

            kernel_restored =
                vmm_test_get_active_address_space(
                    0,
                    &cleanup_active_space,
                    &cleanup_active_root
                ) &&
                cleanup_active_space == kernel_space &&
                cleanup_active_root == kernel_root;
        }
    }

    /*
     * Verify that both process address spaces are inactive
     * before destroying either process.
     */
    bool spaces_inactive = kernel_restored;

    if (space_a != NULL) {

        uint64_t active_mask = UINT64_MAX;

        if (!vmm_test_get_address_space_active_cpu_mask(
                space_a,
                &active_mask
            ) ||
            active_mask != 0) {

            spaces_inactive = false;
        }
    }

    if (space_b != NULL) {

        uint64_t active_mask = UINT64_MAX;

        if (!vmm_test_get_address_space_active_cpu_mask(
                space_b,
                &active_mask
            ) ||
            active_mask != 0) {

            spaces_inactive = false;
        }
    }

    uint64_t cleanup_kernel_mask = 0;

    if (!vmm_test_get_address_space_active_cpu_mask(
            kernel_space,
            &cleanup_kernel_mask
        ) ||
        cleanup_kernel_mask != 1ULL) {

        spaces_inactive = false;
    }

    /*
     * The worker contexts contain borrowed pointers.
     * Clear them before releasing process references.
     */
    for (uint32_t i = 0;
         i < SCHEDULER_CR3_TEST_WORKER_COUNT;
         i++) {

        scheduler_cr3_test_contexts[i] =
            (scheduler_cr3_test_context_t){0};
    }

    /*
     * Only reclaim the process objects if their address
     * spaces are known to be inactive.
     */
    if (spaces_inactive) {

        if (!scheduler_cr3_test_destroy_process(&process_a)) {
            cleanup_ok = false;
        }

        if (!scheduler_cr3_test_destroy_process(&process_b)) {
            cleanup_ok = false;
        }

    } else {

        cleanup_ok = false;
    }

    return passed && cleanup_ok;
}

/*
 * Test nested preemption protection.
 *
 * Verifies:
 * 1. An unmatched enable is rejected.
 * 2. A single disable increments the counter.
 * 3. Nested disables are supported.
 * 4. Each enable releases exactly one level.
 * 5. The counter returns to zero.
 * 6. The running thread is not changed.
 */
static bool scheduler_integration_test_preemption(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap
) {

    if (cpu == NULL ||
        bootstrap == NULL ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        bootstrap->preempt_count != 0) {

        return false;
    }

    /*
     * Enabling without a matching disable must fail.
     */
    if (thread_preempt_enable()) {
        return false;
    }

    if (bootstrap->preempt_count != 0) {
        return false;
    }

    /*
     * First level of protection.
     */
    if (!thread_preempt_disable()) {
        return false;
    }

    if (bootstrap->preempt_count != 1) {
        return false;
    }

    /*
     * Nested protection.
     */
    if (!thread_preempt_disable()) {
        (void)thread_preempt_enable();
        return false;
    }

    if (bootstrap->preempt_count != 2) {
        (void)thread_preempt_enable();
        (void)thread_preempt_enable();
        return false;
    }

    /*
     * Release only one nesting level.
     */
    if (!thread_preempt_enable()) {
        (void)thread_preempt_enable();
        (void)thread_preempt_enable();
        return false;
    }

    if (bootstrap->preempt_count != 1) {
        (void)thread_preempt_enable();
        return false;
    }

    /*
     * Release the final nesting level.
     */
    if (!thread_preempt_enable()) {
        return false;
    }

    if (bootstrap->preempt_count != 0) {
        return false;
    }

    /*
     * Preemption protection must not change the
     * currently running thread.
     */
    if (cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        return false;
    }

    return true;
}

/*
 * Test a complete cooperative thread lifecycle:
 *
 * 1. Create a worker.
 * 2. Enqueue it on CPU0.
 * 3. Yield from the bootstrap thread.
 * 4. Execute the worker.
 * 5. Terminate the worker when its entry returns.
 * 6. Switch back to the bootstrap thread.
 * 7. Reclaim the worker's stack and TCB.
 */
bool scheduler_integration_run_tests(void) {

    /*
     * The scheduler must already be initialized.
     */
    if (!scheduler_is_initialized()) {
        return false;
    }

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        cpu->current_thread == NULL) {

        return false;
    }

    /*
     * Remember the bootstrap thread.
     *
     * Do not retain or inspect the worker pointer after
     * the reaper has destroyed it.
     */
    struct thread *bootstrap =
        cpu->current_thread;

    if (bootstrap->state != THREAD_STATE_RUNNING ||
        bootstrap->scheduling_class == THREAD_CLASS_IDLE) {

        return false;
    }

    /*
    * Verify nested preemption protection before testing
    * the cooperative thread lifecycle.
    */
    if (!scheduler_integration_test_preemption(
            cpu,
            bootstrap
        )) {

        return false;
    }

    /*
     * This test expects a clean CPU0 scheduler state.
     */
    if (cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    /*
     * Save diagnostics so we can verify that exactly
     * two context switches occur:
     *
     * bootstrap -> worker
     * worker -> bootstrap
     */
    uint64_t initial_context_switches =
        cpu->context_switch_count;

    uint64_t initial_voluntary_yields =
        cpu->voluntary_yield_count;

    if (initial_context_switches > UINT64_MAX - 2 ||
        initial_voluntary_yields == UINT64_MAX) {

        return false;
    }

    /*
     * Reset the worker's completion flag.
     */
    worker_executed = false;
    worker_tss_stack_valid = false;

    /*
     * Create the worker.
     */
    struct thread *worker = thread_create(
        "scheduler-test",
        scheduler_integration_worker,
        NULL,
        0
    );

    if (worker == NULL) {
        return false;
    }

    /*
     * Enqueue the worker on CPU0.
     *
     * If insertion fails, it is still a NEW thread
     * and can safely be destroyed.
     */
    if (!scheduler_enqueue_thread(0, worker)) {

        (void)thread_destroy(worker);

        return false;
    }

    /*
     * Cooperatively yield.
     *
     * The worker should execute, return, terminate,
     * and switch execution back to this bootstrap thread.
     */
    if (!thread_yield()) {
        return false;
    }

    /*
     * The worker pointer is now potentially freed.
     *
     * From this point onward, inspect only CPU state
     * and the independent completion flag.
     */

    if (!worker_executed ||
        !worker_tss_stack_valid) {

        return false;
    }

    /*
     * The bootstrap thread must be running again.
     */
    if (cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        return false;
    }

    /*
     * The worker must have been removed from the
     * reaper queue and destroyed.
     */
    if (cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    /*
     * There should be no runnable threads remaining.
     */
    if (cpu->run_queue.runnable_count != 0) {
        return false;
    }

    /*
     * Verify the two expected context switches.
     */
    if (cpu->context_switch_count !=
        initial_context_switches + 2) {

        return false;
    }

    /*
     * Verify one voluntary yield by the bootstrap thread.
     */
    if (cpu->voluntary_yield_count !=
        initial_voluntary_yields + 1) {

        return false;
    }

    /*
     * Test a real BLOCKED -> READY -> RUNNING transition.
     */
    if (!scheduler_integration_test_blocking(
            cpu,
            bootstrap
        )) {

        return false;
    }

    /*
     * Test real scheduler-driven CR3 switching across
     * two independent process address spaces.
     */
    if (!scheduler_integration_test_address_space_switching(
            cpu,
            bootstrap
        )) {

        return false;
    }

    return true;
}
