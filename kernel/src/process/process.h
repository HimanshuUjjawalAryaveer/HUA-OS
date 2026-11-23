#ifndef HUA_PROCESS_H
#define HUA_PROCESS_H

#include <stdbool.h>
#include <stdint.h>
#include "mm/vmm.h"

#define PROCESS_NAME_MAX 32U

typedef uint64_t process_id_t;

typedef enum {
    PROCESS_STATE_NEW = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_EXITING,
    PROCESS_STATE_ZOMBIE
} process_state_t;

typedef enum {
    PROCESS_TYPE_KERNEL = 0,
    PROCESS_TYPE_USER
} process_type_t;

typedef struct process process_t;
struct thread;

/*
 * HUA process-level permissions for an image page.
 *
 * These are independent of the ELF64 PF_R/PF_W/PF_X encodings.
 * The ELF loader must translate each permission explicitly.
 */
#define PROCESS_IMAGE_PAGE_READ       0x1U
#define PROCESS_IMAGE_PAGE_WRITE      0x2U
#define PROCESS_IMAGE_PAGE_EXECUTE    0x4U

/*
 * Ownership record for one physical page belonging to a process.
 *
 * frame_owned:
 *     The process still owns the physical frame.
 *
 * mapped:
 *     The frame is currently mapped at virtual_address.
 *
 * quarantined:
 *     Cleanup encountered an ownership inconsistency.
 *     The frame must not be freed automatically.
 *
 * unexpected_physical_address:
 *     Physical address returned by VMM if unmapping returned
 *     a frame different from the one recorded here.
 */
typedef struct process_owned_page {

    uint64_t virtual_address;
    uint64_t physical_address;

    uint64_t unexpected_physical_address;

    uint32_t permissions;

    bool frame_owned;
    bool mapped;
    bool quarantined;

} process_owned_page_t;

/*
 * One contiguous allocation containing all page records
 * for a process-owned memory region.
 *
 * The process owns this structure after it is successfully
 * attached using process_image_load_attach_pages().
 */
typedef struct process_owned_page_set {

    struct process_owned_page_set *next;

    uint64_t page_count;

    bool attached;
    bool cleanup_in_progress;

    process_owned_page_t pages[];

} process_owned_page_set_t;

/*
 * First unhandled user-mode exception recorded for a process.
 *
 * This record belongs to the process rather than the thread,
 * so it remains available after the scheduler reaps the TCB.
 */
typedef struct process_exception_info {

    bool valid;

    uint64_t thread_id;
    uint64_t vector;
    uint64_t error_code;

    uint64_t instruction_pointer;
    uint64_t fault_address;

} process_exception_info_t;

/*
 * Retrieve the process address space.
 *
 * The returned address-space pointer is borrowed.
 * The caller must hold a process reference for the entire
 * period during which the pointer is used.
 *
 * The process owns the address-space reference.
 */
bool process_get_address_space(
    process_t *process,
    vmm_address_space_t **address_space_out
);

/*
 * Reserve a NEW user process for executable-image construction.
 *
 * The returned address-space pointer is borrowed.
 * The caller must continue holding a process reference.
 *
 * The process cannot be loaded twice or started while
 * image construction is in progress.
 */
bool process_image_load_begin(
    process_t *process,
    vmm_address_space_t **address_space_out
);

/*
 * Transfer ownership of a prepared page set to a process
 * whose image load is currently in progress.
 *
 * On success:
 *     The process owns the page set and all recorded frames.
 *
 * On failure:
 *     Ownership remains with the caller.
 *
 * Requirements:
 *     Physical addresses must be strictly increasing.
 *     Every frame must be allocated by PMM.
 *     Each page must initially be owned but not mapped.
 */
bool process_image_load_attach_pages(
    process_t *process,
    process_owned_page_set_t *page_set
);

/*
 * Map one page from an attached process-owned image page set.
 *
 * The process manager:
 *   - Validates process and page-set ownership.
 *   - Converts process image permissions into VMM flags.
 *   - Creates the mapping.
 *   - Marks the ownership record mapped only after VMM succeeds.
 *
 * The operation is serialized by the process lock. The process
 * must still be in its image-loading phase.
 *
 * page_set remains a borrowed pointer after attachment. The caller
 * must retain a process reference and must not use the pointer after
 * the page set has been released.
 */
bool process_image_load_map_page(
    process_t *process,
    process_owned_page_set_t *page_set,
    uint64_t page_index
);

/*
 * Finish image construction.
 *
 * success=true requires every attached page to be mapped
 * and owned by the process.
 *
 * success=false marks the load as failed if cleanup records
 * remain attached to the process.
 */
bool process_image_load_finish(
    process_t *process,
    bool success
);

/*
 * Return true when a registered user process has a committed ELF image.
 */
bool process_image_is_loaded(process_t *process);

/*
 * Reserve a loaded, NEW user process for construction of its initial
 * user stack. The returned address space is borrowed.
 */
bool process_user_start_begin(
    process_t *process,
    vmm_address_space_t **address_space_out
);

/*
 * Commit or abort initial user-stack construction.
 *
 * On success, stack_base and stack_top describe the complete mapped
 * user stack. The manager verifies its owned pages, effective
 * permissions, and unmapped guard page.
 *
 * On failure, the transaction is closed but marked failed. The caller
 * must release every page it allocated and then call
 * process_user_start_reset_failed().
 */
bool process_user_start_finish(
    process_t *process,
    bool success,
    uint64_t stack_base,
    uint64_t stack_top
);

/*
 * Clear a failed initial-stack transaction after all pages belonging
 * to that transaction have been released.
 *
 * This function refuses to reset while any process-owned page record
 * remains inside the failed stack range.
 */
bool process_user_start_reset_failed(process_t *process);

/*
 * Retrieve the initial user-stack bounds of a process whose stack is
 * ready or whose initial user thread has already been attached.
 */
bool process_get_user_stack(
    process_t *process,
    uint64_t *stack_base_out,
    uint64_t *stack_top_out
);

/*
 * Roll back the initial thread attachment if scheduler enqueue fails.
 *
 * This is only valid for the sole NEW thread of a newly started
 * image-loaded user process.
 */
bool process_cancel_initial_thread(
    process_t *process,
    struct thread *thread
);

/*
 * Release one page set already owned by the process.
 *
 * Success:
 *     All mappings are removed, all frames are freed,
 *     and the page-set metadata is freed.
 *
 * Failure:
 *     Remaining ownership records stay attached to the
 *     process for later cleanup attempts.
 */
bool process_release_owned_pages(
    process_t *process,
    process_owned_page_set_t *page_set
);

/*
 * Initialize the process manager.
 * Must be called after heap initialization.
 */
bool process_init(void);

/*
 * Create a registered kernel process.
 *
 * The returned pointer owns one reference.
 * Release it with process_put().
 */
process_t *process_create_kernel(const char *name);

/*
 * Create a registered user process with a new, independent
 * address space.
 *
 * The returned pointer owns one process reference.
 * Release it with process_put().
 *
 * The process owns the initial VMM address-space reference.
 */
process_t *process_create_user(const char *name);

/*
 * Find a process by PID.
 *
 * Returns a referenced process object.
 * Release it with process_put().
 */
process_t *process_get_by_pid(process_id_t pid);

/*
 * Release a reference to a process.
 */
void process_put(process_t *process);

/*
 * Mark a process as running.
 */
bool process_set_running(process_t *process);

/*
 * Begin process termination.
 *
 * The process becomes a zombie immediately if it
 * has no attached threads.
 */
bool process_begin_exit(process_t *process);

/*
 *   - The calling thread has preemption enabled on entry
 *     (preempt_count == 0).
 * Begin process termination on behalf of its calling thread.
 *
 * Initial implementation requires:
 *   - A registered user process in RUNNING state.
 *   - Exactly one attached thread.
 *   - The supplied thread to be that sole attached thread.
 *   - The thread to be RUNNING.
 *
 * The checks and RUNNING -> EXITING transition occur
 * atomically under process_lock.
 *
 * Failure leaves the process unchanged.
 *
 * The caller must hold a valid process reference.
 */
bool process_begin_exit_from_thread(
    process_t *process,
    struct thread *thread
);

/*
 * Attach a NEW, detached thread to a process.
 *
 * This operation:
 *   - Acquires one process reference.
 *   - Adds the TCB to the process-owned thread list.
 *   - Sets thread->process and thread->address_space.
 *   - Increments the process thread count.
 *
 * The caller must hold a valid process reference.
 */
bool process_attach_thread(
    process_t *process,
    struct thread *thread
);

/*
 * Detach a process-owned thread that has never started running.
 *
 * This removes its list membership, clears its process and
 * address-space pointers, and releases its process reference.
 */
bool process_detach_thread(
    process_t *process,
    struct thread *thread
);

/*
 * Account for a permanently terminated process-owned thread.
 *
 * Called by the deferred scheduler reaper, after the thread
 * has reached THREAD_STATE_TERMINATED.
 *
 * Removes list membership and releases the thread's process
 * reference. The final thread makes the process a ZOMBIE.
 */
bool process_thread_exit(
    process_t *process,
    struct thread *thread
);

/*
 * Record an unhandled user-mode exception and begin process exit.
 *
 * Initial implementation deliberately supports only a
 * single-threaded user process. It rejects multi-threaded
 * processes rather than leaving sibling threads running.
 *
 * The supplied thread must be attached to this process and
 * must be its only RUNNING thread.
 */
bool process_record_user_exception(
    process_t *process,
    struct thread *thread,
    const process_exception_info_t *information
);


/*
 * Retrieve the process's recorded user-mode exception.
 *
 * The process must still be registered and referenced by
 * the caller. The record survives thread reclamation.
 */
bool process_get_exception_info(
    process_t *process,
    process_exception_info_t *information_out
);

/*
 * Remove a process from the global registry.
 *
 * Only NEW or ZOMBIE processes with no attached
 * threads can be destroyed.
 *
 * The caller must still release its own reference
 * using process_put().
 */
bool process_destroy(process_t *process);

/*
 * Read process information.
 */
bool process_get_info(
    process_t *process,
    process_id_t *pid,
    process_state_t *state,
    uint32_t *thread_count,
    char *name,
    uint32_t name_capacity
);

/*
 * Retrieve the final process exit status.
 *
 * The process must still be registered, be in ZOMBIE state,
 * and have no attached threads.
 *
 * The caller must hold a valid process reference.
 */
bool process_get_exit_code(
    process_t *process,
    int64_t *exit_code_out
);

#endif
