
#ifndef HUA_THREAD_H
#define HUA_THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mm/vmm.h"

#include "arch/x86_64/context.h"

/*
 * Maximum length of a thread's human-readable name,
 * including the terminating null character.
 */
#define THREAD_NAME_MAX 32U

#define THREAD_STACK_DEFAULT_SIZE (16ULL * 1024ULL)
#define THREAD_STACK_MIN_SIZE     (16ULL * 1024ULL)

/*
 * Number of real-time priority levels.
 *
 * Priority 31 is the highest.
 * Priority 0 is the lowest real-time priority.
 */
#define THREAD_RT_PRIORITY_LEVELS 32U

/*
 * Real-time round-robin quantum policy.
 *
 * The LAPIC scheduler timer currently runs at 100 Hz,
 * giving a scheduling resolution of approximately 10 ms.
 */
#define THREAD_RT_DEFAULT_QUANTUM_NS 20000000ULL
#define THREAD_RT_QUANTUM_MIN_NS     10000000ULL
#define THREAD_RT_QUANTUM_MAX_NS     1000000000ULL

/*
 * EEVDF fair scheduling configuration.
 */
#define THREAD_FAIR_WEIGHT_SCALE          1024ULL
#define THREAD_FAIR_WEIGHT_DEFAULT        1024ULL
#define THREAD_FAIR_WEIGHT_MIN            1ULL
#define THREAD_FAIR_WEIGHT_MAX            1048576ULL

#define THREAD_FAIR_DEFAULT_SLICE_NS      6000000ULL

/*
 * Thread identifier.
 *
 * Zero is reserved as an invalid identifier.
 */
typedef uint64_t thread_id_t;

/*
 * Thread lifecycle states.
 */
typedef enum {

    THREAD_STATE_NEW = 0,

    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,

    THREAD_STATE_BLOCKED,
    THREAD_STATE_SLEEPING,

    THREAD_STATE_TERMINATED

} thread_state_t;

/*
 * Scheduling classes.
 *
 * The scheduler selects work in this class order:
 *
 * 1. Real-time
 * 2. Fair
 * 3. Idle
 */
typedef enum {

    THREAD_CLASS_REALTIME = 0,

    THREAD_CLASS_FAIR,

    THREAD_CLASS_IDLE

} thread_class_t;

/*
 * Real-time scheduling policies.
 */
typedef enum {

    THREAD_RT_FIFO = 0,

    THREAD_RT_ROUND_ROBIN

} thread_rt_policy_t;

/*
 * Thread entry function.
 */
typedef void (*thread_entry_t)(void *argument);

struct scheduler_run_queue;

struct wait_queue;

struct process;

/*
 * Kernel thread control block.
 *
 * This structure represents one independently schedulable
 * execution context.
 */
struct thread {

    /*
     * Identity and debugging information.
     */
    thread_id_t id;

    char name[THREAD_NAME_MAX];

    /*
     * Owning process.
     *
     * NULL means this is a process-independent kernel thread
     * or scheduler infrastructure thread.
     *
     * A non-NULL pointer represents one process thread reference.
     */
    struct process *process;

    /*
    * Borrowed address space cached at thread creation.
    *
    * Process-owned threads use their process's address space.
    * This pointer is protected by the process reference held
    * by the thread.
    *
    * NULL for process-independent kernel threads.
    *
    * Never release this pointer directly from the TCB.
    */
    vmm_address_space_t *address_space;

    /*
    * Intrusive process-thread list.
    *
    * Every attached thread belongs to exactly one process list.
    * These links are protected by the owning process lock.
    *
    * NULL for process-independent kernel threads.
    */
    struct thread *process_previous;

    struct thread *process_next;

    /*
     * Lifecycle and scheduling classification.
     */
    thread_state_t state;

    thread_class_t scheduling_class;

    /*
     * Architecture-specific cooperative context.
     */
    struct arch_thread_context context;

    /*
     * Kernel stack information.
     *
     * stack_base:
     *     Beginning of the allocated stack region.
     *
     * stack_size:
     *     Total stack region size.
     *
     * stack_top:
     *     Initial top of the stack, before alignment
     *     and initial context construction.
     *
     * stack_owned:
     *     True when the scheduler owns this stack and
     *     may eventually release it.
     *
     * The bootstrap thread will use a borrowed stack.
     */
    void *stack_base;

    size_t stack_size;

    uintptr_t stack_top;

    bool stack_owned;

    /*
     * Entry function and argument.
     *
     * These are used when constructing a new thread's
     * initial execution context.
     */
    thread_entry_t entry;

    void *entry_argument;

    /*
     * Real-time scheduling information.
     */
    uint8_t realtime_priority;

    thread_rt_policy_t realtime_policy;

    uint64_t realtime_quantum_ns;

    /*
     * EEVDF fair scheduling information.
     *
     * fair_weight:
     *     Relative CPU share. 1024 is the baseline.
     *
     * virtual_runtime:
     *     Execution time normalized by fair_weight.
     *
     * virtual_deadline:
     *     Virtual-time deadline of the current request.
     *
     * fair_slice_ns:
     *     Length of the scheduling request in real nanoseconds.
     */
    uint64_t fair_weight;

    uint64_t virtual_runtime;

    uint64_t virtual_deadline;

    uint64_t fair_slice_ns;

    /*
    * Runtime accounting.
    *
    * runtime_ns:
    *     Total CPU execution time accumulated over the
    *     lifetime of this thread.
    *
    * last_start_ns:
    *     Monotonic timestamp at the beginning of the
    *     current execution interval.
    *
    * slice_runtime_ns:
    *     CPU execution time consumed in the current
    *     scheduling slice.
    */
    uint64_t runtime_ns;

    uint64_t last_start_ns;

    uint64_t slice_runtime_ns;

    int64_t exit_code;

    /*
     * CPU affinity.
     *
     * Bit N represents permission to execute on
     * logical CPU N.
     *
     * Initially only CPU 0 will be active.
     */
    uint64_t affinity_mask;

    uint32_t last_cpu;

    /*
     * Nested preemption-disable count.
     *
     * A nonzero value means this thread must not
     * be involuntarily preempted at that moment.
     */
    uint32_t preempt_count;

    /*
     * Intrusive run-queue links.
     *
     * A thread may belong to at most one run queue
     * at a time.
     */
    struct thread *run_previous;

    struct thread *run_next;

    /*
    * Queue ownership prevents accidental insertion into
    * multiple CPU run queues.
    */
    struct scheduler_run_queue *run_queue_owner;

    /*
     * Wait-queue linkage.
     *
     * Separate from run-queue linkage because a
     * blocked thread must not remain runnable.
     */
    struct thread *wait_previous;

    struct thread *wait_next;

    void *wait_channel;

    /*
    * Timed-sleep queue linkage.
    *
    * A sleeping thread belongs to at most one sleep queue.
    * The queue is ordered by sleep_deadline_ns.
    */
    struct thread *sleep_previous;

    struct thread *sleep_next;

    uint64_t sleep_deadline_ns;

    bool sleep_queued;

    /*
     * Deferred thread-reclamation linkage.
     *
     * A terminated thread cannot release its own stack
     * while executing on that stack.
     *
     * The scheduler places terminated threads on a
     * separate reclamation list. Another thread later
     * releases their resources.
     */
    struct thread *reap_next;

    /*
     * Prevent duplicate insertion into the reclamation list.
     */
    bool reap_pending;

    bool reap_queued;

};

/*
 * Create a new thread object and allocate its kernel stack.
 *
 * The thread begins in THREAD_STATE_NEW.
 * It is not placed in a run queue and cannot execute yet.
 *
 * stack_size == 0 selects THREAD_STACK_DEFAULT_SIZE.
 */
struct thread *thread_create(
    const char *name,
    thread_entry_t entry,
    void *argument,
    size_t stack_size
);

/*
 * Create a thread owned by a process.
 *
 * The caller must hold a valid reference to the process
 * while calling this function.
 *
 * The thread begins in THREAD_STATE_NEW and is not runnable.
 * Successful creation attaches it to the process and acquires
 * a process reference on behalf of the thread.
 *
 * Destroying an unstarted thread releases that attachment.
 * A terminated thread releases it through the scheduler reaper.
 */
struct thread *thread_create_for_process(
    struct process *process,
    const char *name,
    thread_entry_t entry,
    void *argument,
    size_t stack_size
);

/*
 * Configure a detached NEW thread as a real-time thread.
 *
 * priority:
 *   0 is the lowest RT priority.
 *   THREAD_RT_PRIORITY_LEVELS - 1 is the highest.
 *
 * FIFO:
 *   quantum_ns must be zero because FIFO does not rotate
 *   automatically between equal-priority threads.
 *
 * ROUND_ROBIN:
 *   quantum_ns == 0 selects the default quantum.
 *   Otherwise, it must be within the supported range.
 *
 * This operation is only valid before the thread is
 * inserted into a run queue.
 */
bool thread_set_realtime(
    struct thread *thread,
    uint8_t priority,
    thread_rt_policy_t policy,
    uint64_t quantum_ns
);

/*
 * Adopt the currently executing kernel context as the
 * bootstrap thread.
 *
 * Its existing stack is borrowed. Its bounds are unknown,
 * so stack_base, stack_size, and stack_top are not fabricated.
 */
struct thread *thread_adopt_bootstrap(const char *name);

/*
 * Destroy a thread that has not started running or has terminated.
 *
 * Running, ready, blocked, and sleeping threads cannot
 * be destroyed through this function.
 */
bool thread_destroy(struct thread *thread);

/*
 * Voluntarily give the CPU to another runnable thread.
 *
 * Returns true if the yield operation completed.
 * Returns false if scheduling could not safely proceed.
 *
 * This must be called from ordinary thread context,
 * not from an interrupt handler.
 */
bool thread_yield(void);

/*
 * Block the currently running thread on a wait queue.
 *
 * The thread remains blocked until another thread wakes it.
 * When this function returns true, the thread has resumed.
 *
 * Must be called from ordinary thread context with
 * preemption enabled.
 */
bool thread_block(struct wait_queue *queue);

/*
 * Wake a thread currently blocked on a wait queue.
 *
 * The thread is removed from its wait queue and inserted
 * into the CPU0 runnable queue.
 *
 * Returns false if the thread cannot be woken.
 */
bool thread_wake(struct thread *thread);

/*
 * Sleep for a relative duration in nanoseconds.
 *
 * Returns true after the thread has resumed.
 * A zero duration returns immediately.
 */
bool thread_sleep(uint64_t duration_ns);

/*
 * Sleep until an absolute monotonic-clock deadline.
 *
 * A deadline that has already passed returns immediately.
 */
bool thread_sleep_until(uint64_t deadline_ns);

/*
 * Temporarily prevent involuntary preemption of the
 * currently running thread.
 *
 * Calls may be nested. Every successful disable must
 * eventually have a matching enable.
 *
 * Returns false if the operation cannot be performed.
 */
bool thread_preempt_disable(void);


/*
 * Release one level of preemption protection.
 *
 * A pending reschedule will be handled by the scheduler
 * when preemption becomes safe.
 *
 * Returns false if preemption was not disabled.
 */
bool thread_preempt_enable(void);

/*
 * Terminate the currently running thread.
 *
 * This function never returns.
 */
_Noreturn void thread_exit(int64_t exit_code);

#endif
