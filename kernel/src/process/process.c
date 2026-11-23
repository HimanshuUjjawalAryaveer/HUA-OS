#include <stddef.h>
#include <stdint.h>
#include "process/process.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sync/raw_spinlock.h"
#include "scheduler/thread.h"

struct process {

    process_id_t pid;

    char name[PROCESS_NAME_MAX];

    process_type_t type;

        /*
     * Virtual address space used by every thread
     * belonging to this process.
     */
    vmm_address_space_t *address_space;

    /*
     * True when this process owns a private address space
     * that must eventually be released with vmm_address_space_put().
     *
     * Kernel processes borrow the permanent kernel address space.
     */
    bool owns_address_space;

    /*
    * Physical memory owned by this process.
    *
    * Each page set owns its records and physical frames.
    * Page-table frames remain owned by VMM.
    */
    process_owned_page_set_t *owned_page_sets;

    /*
    * Executable-image construction state.
    */
    bool image_loading;
    bool image_loaded;
    bool image_load_failed;

    /*
     * Initial user-process startup transaction.
     *
     * user_start_in_progress:
     *     A kernel caller is creating and mapping the initial
     *     user stack. No thread may be attached during this phase.
     *
     * user_start_ready:
     *     The image and initial stack are fully committed.
     *     The first process-owned thread may now be attached.
     *
     * user_start_failed:
     *     Stack construction failed and its ownership cleanup
     *     has not yet been confirmed complete.
     *
     * user_started:
     *     The initial process-owned thread has been attached.
     */
    bool user_start_in_progress;
    bool user_start_ready;
    bool user_start_failed;
    bool user_started;

    uint64_t user_stack_base;
    uint64_t user_stack_top;

    /*
    * Protects final process cleanup from concurrent attempts.
    */
    bool cleanup_in_progress;

    process_state_t state;

    /*
     * Final process termination status.
     *
     * When the final attached thread is reaped, its
     * thread->exit_code is copied here.
     *
     * This survives destruction of the final thread TCB.
     *
     * A process that becomes ZOMBIE without a final thread
     * exit has the default status zero.
     *
     * Protected by process_lock.
     */
    int64_t exit_code;

    /*
    * Durable record of the first unhandled user exception.
    *
    * Protected by process_lock.
    */
    process_exception_info_t exception_info;

    /*
    * Intrusive list of all threads attached to this process.
    *
    * Protected by process_lock.
    */
    struct thread *thread_head;

    uint32_t thread_count;

    /*
     * References held by callers and attached threads,
     * plus the registry's own reference.
     */
    uint64_t reference_count;

    bool registered;

    struct process *registry_next;
};


/*
 * Global process manager.
 */
static raw_spinlock_t process_lock;

static process_t *process_registry = NULL;

static process_id_t process_next_pid = 1;

static bool process_manager_initialized = false;


/*
 * Acquire the process registry lock.
 *
 * This raw IRQ-save lock does not depend on scheduler
 * preemption state. It is suitable for protecting the
 * process registry and its ownership metadata.
 */
static bool process_lock_acquire(uint64_t *flags) {

    if (flags == NULL) {
        return false;
    }

    return raw_spinlock_lock_irqsave(
        &process_lock,
        flags
    );
}


/*
 * Release the process registry lock and restore the
 * caller's original interrupt state.
 */
static bool process_lock_release(uint64_t flags) {

    return raw_spinlock_unlock_irqrestore(
        &process_lock,
        flags
    );
}

/*
 * Validate one thread's local membership in a process list.
 *
 * Caller must hold process_lock.
 */
static bool process_thread_membership_valid_unlocked(
    process_t *process,
    struct thread *thread
) {

    if (process == NULL ||
        thread == NULL ||
        thread->process != process ||
        thread->address_space != process->address_space) {

        return false;
    }

    /*
     * Validate the previous link or list head.
     */
    if (thread->process_previous != NULL) {

        if (thread->process_previous->process_next != thread) {
            return false;
        }

    } else {

        if (process->thread_head != thread) {
            return false;
        }
    }

    /*
     * Validate the next link.
     */
    if (thread->process_next != NULL) {

        if (thread->process_next->process_previous != thread) {
            return false;
        }
    }

    return true;
}


/*
 * Remove a validated thread from its process list.
 *
 * Caller must hold process_lock.
 *
 * This function also clears all borrowed ownership pointers.
 */
static void process_thread_unlink_unlocked(
    process_t *process,
    struct thread *thread
) {

    if (thread->process_previous != NULL) {

        thread->process_previous->process_next =
            thread->process_next;

    } else {

        process->thread_head = thread->process_next;
    }

    if (thread->process_next != NULL) {

        thread->process_next->process_previous =
            thread->process_previous;
    }

    thread->process_previous = NULL;
    thread->process_next = NULL;

    thread->process = NULL;
    thread->address_space = NULL;
}

/*
 * Copy a process name into its fixed-size buffer.
 */
static bool process_copy_name(
    char *destination,
    const char *source
) {

    if (destination == NULL ||
        source == NULL) {
        return false;
    }

    uint32_t i = 0;

    while (i < PROCESS_NAME_MAX - 1) {

        destination[i] = source[i];

        if (source[i] == '\0') {
            return true;
        }

        i++;
    }

    /*
     * Ensure termination even for long names.
     */
    destination[PROCESS_NAME_MAX - 1] = '\0';

    return true;
}


/*
 * Allocate the next process identifier.
 *
 * PID zero is invalid. Once the identifier space
 * is exhausted, creation fails instead of wrapping.
 *
 * Caller must hold process_lock.
 */
static process_id_t process_allocate_pid(void) {

    if (process_next_pid == 0) {
        return 0;
    }

    process_id_t pid = process_next_pid;

    if (process_next_pid == UINT64_MAX) {
        process_next_pid = 0;
    } else {
        process_next_pid++;
    }

    return pid;
}


/*
 * Find a registered process.
 *
 * Caller must hold process_lock.
 */
static process_t *process_find_unlocked(
    process_id_t pid
) {

    if (pid == 0) {
        return NULL;
    }

    process_t *current = process_registry;

    while (current != NULL) {

        if (current->pid == pid &&
            current->registered) {

            return current;
        }

        current = current->registry_next;
    }

    return NULL;
}


/*
 * Initialize the process manager.
 */
bool process_init(void) {

    if (process_manager_initialized) {
        return false;
    }

    if (!raw_spinlock_init(&process_lock)) {
        return false;
    }

    process_registry = NULL;
    process_next_pid = 1;

    process_manager_initialized = true;

    return true;
}


/*
 * Create, initialize and register a process.
 *
 * The caller supplies the address space.
 *
 * On success:
 *   - The registry owns one process reference.
 *   - The caller owns one process reference.
 *
 * On failure:
 *   - The process descriptor is freed.
 *   - The supplied address space remains caller-owned.
 */
static process_t *process_create_registered(
    const char *name,
    process_type_t type,
    vmm_address_space_t *address_space,
    bool owns_address_space
) {
    if (!process_manager_initialized ||
        name == NULL ||
        name[0] == '\0' ||
        address_space == NULL ||
        (type != PROCESS_TYPE_KERNEL &&
         type != PROCESS_TYPE_USER) ||
        (type == PROCESS_TYPE_KERNEL &&
         owns_address_space) ||
        (type == PROCESS_TYPE_USER &&
         !owns_address_space)) {

        return NULL;
    }

    process_t *process = kcalloc(
        1,
        sizeof(process_t)
    );

    if (process == NULL) {
        return NULL;
    }

    if (!process_copy_name(
            process->name,
            name
        )) {

        kfree(process);
        return NULL;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        kfree(process);
        return NULL;
    }

    process_id_t pid = process_allocate_pid();

    if (pid == 0) {

        process_lock_release(flags);

        kfree(process);
        return NULL;
    }

    process->pid = pid;
    process->type = type;

    process->address_space = address_space;
    process->owns_address_space = owns_address_space;

    process->state = PROCESS_STATE_NEW;
    process->exit_code = 0;
    process->thread_count = 0;
    process->thread_head = NULL;

    process->exception_info =
        (process_exception_info_t){0};

    /*
     * One reference belongs to the registry.
     * One reference belongs to the caller.
     */
    process->reference_count = 2;

    process->registered = true;

    process->registry_next = process_registry;
    process_registry = process;

    process_lock_release(flags);

    return process;
}


/*
 * Create a registered kernel process.
 *
 * Kernel processes borrow the permanent kernel
 * address space and do not own a separate VMM object.
 */
process_t *process_create_kernel(
    const char *name
) {
    if (!process_manager_initialized) {
        return NULL;
    }

    vmm_address_space_t *address_space =
        vmm_kernel_address_space();

    if (address_space == NULL) {
        return NULL;
    }

    return process_create_registered(
        name,
        PROCESS_TYPE_KERNEL,
        address_space,
        false
    );
}


/*
 * Create a registered user process with an independent
 * private address space.
 *
 * This creates the address-space foundation only.
 * Loading an executable and entering user mode are
 * separate future operations.
 */
process_t *process_create_user(
    const char *name
) {
    if (!process_manager_initialized ||
        name == NULL ||
        name[0] == '\0') {

        return NULL;
    }

    /*
     * Create the private address space first.
     *
     * vmm_address_space_create() allocates its private
     * PML4 root and shares the supervisor-only kernel
     * upper-half mappings.
     */
    vmm_address_space_t *address_space =
        vmm_address_space_create();

    if (address_space == NULL) {
        return NULL;
    }

    /*
     * Register the process and transfer ownership
     * of the address space only on success.
     */
    process_t *process = process_create_registered(
        name,
        PROCESS_TYPE_USER,
        address_space,
        true
    );

    if (process == NULL) {

        /*
         * Registration failed.
         * The process helper did not consume the
         * caller's address-space reference.
         */
        vmm_address_space_put(address_space);

        return NULL;
    }

    return process;
}

/*
 * Look up a process and acquire a reference.
 */
process_t *process_get_by_pid(
    process_id_t pid
) {

    if (!process_manager_initialized ||
        pid == 0) {

        return NULL;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return NULL;
    }

    process_t *process =
        process_find_unlocked(pid);

    if (process != NULL) {

        if (process->reference_count == UINT64_MAX) {
            process = NULL;
        } else {
            process->reference_count++;
        }
    }

    process_lock_release(flags);

    return process;
}

/*
 * Retrieve the address space belonging to a process.
 *
 * The caller must hold a process reference.
 * The returned pointer is borrowed, not an additional
 * VMM address-space reference.
 */
bool process_get_address_space(
    process_t *process,
    vmm_address_space_t **address_space_out
) {
    if (!process_manager_initialized ||
        process == NULL ||
        address_space_out == NULL) {

        return false;
    }

    *address_space_out = NULL;

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool result = false;

    if (process->reference_count > 0 &&
        process->address_space != NULL &&
        !process->cleanup_in_progress) {

        *address_space_out = process->address_space;
        result = true;
    }

    process_lock_release(flags);

    return result;
}

/*
 * Reserve a NEW user process for image construction.
 *
 * The process remains NEW and cannot be started until
 * process_image_load_finish() completes successfully.
 */
bool process_image_load_begin(
    process_t *process,
    vmm_address_space_t **address_space_out
) {
    if (!process_manager_initialized ||
        process == NULL ||
        address_space_out == NULL) {

        return false;
    }

    *address_space_out = NULL;

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        !process->user_start_in_progress &&
        !process->user_start_ready &&
        !process->user_start_failed &&
        !process->user_started &&

        process->registered &&
        process->reference_count > 0 &&
        process->type == PROCESS_TYPE_USER &&
        process->owns_address_space &&
        process->address_space != NULL &&
        process->state == PROCESS_STATE_NEW &&
        process->thread_count == 0 &&
        process->thread_head == NULL &&
        process->owned_page_sets == NULL &&
        !process->image_loading &&
        !process->image_loaded &&
        !process->image_load_failed &&
        !process->cleanup_in_progress;

    if (valid) {
        process->image_loading = true;
        *address_space_out = process->address_space;
    }

    process_lock_release(flags);

    return valid;
}


/*
 * Attach a prepared page set to a process.
 *
 * The process takes ownership of the metadata and every
 * physical frame only when this function succeeds.
 *
 * The records must be sorted by physical address so that
 * duplicate frame ownership can be rejected efficiently.
 */
bool process_image_load_attach_pages(
    process_t *process,
    process_owned_page_set_t *page_set
) {
    if (!process_manager_initialized ||
        process == NULL ||
        page_set == NULL ||
        page_set->page_count == 0 ||
        page_set->attached ||
        page_set->cleanup_in_progress ||
        page_set->next != NULL) {

        return false;
    }

    uint64_t previous_physical = 0;

    /*
     * Validate every record before transferring ownership.
     *
     * Strictly increasing physical addresses reject
     * duplicate frame ownership without quadratic checking.
     */
    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        process_owned_page_t *page =
            &page_set->pages[i];

        if (page->virtual_address < 0x10000ULL ||
            page->virtual_address >=
                0x0000800000000000ULL ||
            (page->virtual_address & 0xFFFULL) != 0 ||
            page->physical_address == 0 ||
            (page->physical_address & 0xFFFULL) != 0 ||
            !page->frame_owned ||
            page->mapped ||
            page->quarantined ||
            page->unexpected_physical_address != 0) {

            return false;
        }

        if ((page->permissions &
                ~(PROCESS_IMAGE_PAGE_READ |
                  PROCESS_IMAGE_PAGE_WRITE |
                  PROCESS_IMAGE_PAGE_EXECUTE)) != 0 ||
            !(page->permissions & PROCESS_IMAGE_PAGE_READ) ||
            ((page->permissions & PROCESS_IMAGE_PAGE_WRITE) &&
             (page->permissions & PROCESS_IMAGE_PAGE_EXECUTE))) {

            return false;
        }

        if (i != 0 &&
            page->physical_address <= previous_physical) {

            return false;
        }

        /*
         * Confirm that PMM recognizes this as an allocated
         * frame before accepting ownership.
         */
        if (pmm_frame_to_virt(
                page->physical_address
            ) == NULL) {

            return false;
        }

        previous_physical = page->physical_address;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->reference_count > 0 &&
        process->type == PROCESS_TYPE_USER &&
        process->owns_address_space &&
        process->address_space != NULL &&
        process->state == PROCESS_STATE_NEW &&
        (process->image_loading ||
        (process->user_start_in_progress &&
        process->image_loaded)) &&
        !process->image_load_failed &&
        !process->cleanup_in_progress;

    if (valid) {

        page_set->next = process->owned_page_sets;
        process->owned_page_sets = page_set;

        /*
         * Ownership transfers to the process here.
         */
        page_set->attached = true;
    }

    process_lock_release(flags);

    return valid;
}

/*
 * Map one process-owned image page.
 *
 * The process lock remains held across the VMM mapping operation.
 * This makes the PTE change and the ownership-record update one
 * indivisible operation with respect to process cleanup.
 */
bool process_image_load_map_page(
    process_t *process,
    process_owned_page_set_t *page_set,
    uint64_t page_index
) {
    if (!process_manager_initialized ||
        process == NULL ||
        page_set == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    /*
     * Verify that this exact page set is still owned by
     * the specified process before dereferencing it.
     */
    bool found = false;

    for (process_owned_page_set_t *current =
             process->owned_page_sets;
         current != NULL;
         current = current->next) {

        if (current == page_set) {
            found = true;
            break;
        }
    }

    if (!found ||
        !page_set->attached ||
        page_set->cleanup_in_progress ||
        page_index >= page_set->page_count ||
        !process->registered ||
        process->reference_count == 0 ||
        process->type != PROCESS_TYPE_USER ||
        !process->owns_address_space ||
        process->address_space == NULL ||
        process->state != PROCESS_STATE_NEW ||
        !(process->image_loading ||
        (process->user_start_in_progress &&
        process->image_loaded)) ||
        process->image_load_failed ||
        process->cleanup_in_progress) {

        process_lock_release(flags);
        return false;
    }

    process_owned_page_t *page =
        &page_set->pages[page_index];

    /*
     * Only an allocated, currently unmapped, non-quarantined
     * frame can be mapped through this operation.
     */
    if (!page->frame_owned ||
        page->mapped ||
        page->quarantined ||
        page->unexpected_physical_address != 0) {

        process_lock_release(flags);
        return false;
    }

    /*
     * Validate and translate the process-level permissions.
     *
     * Do not pass ELF PF_* bits directly to VMM:
     * the ELF and process permission bit assignments differ.
     */
    const uint32_t allowed_permissions =
        PROCESS_IMAGE_PAGE_READ |
        PROCESS_IMAGE_PAGE_WRITE |
        PROCESS_IMAGE_PAGE_EXECUTE;

    if ((page->permissions & ~allowed_permissions) != 0 ||
        !(page->permissions & PROCESS_IMAGE_PAGE_READ) ||
        ((page->permissions & PROCESS_IMAGE_PAGE_WRITE) &&
         (page->permissions & PROCESS_IMAGE_PAGE_EXECUTE))) {

        process_lock_release(flags);
        return false;
    }

    uint64_t vmm_flags = VMM_MAP_USER;

    if (page->permissions & PROCESS_IMAGE_PAGE_WRITE) {
        vmm_flags |= VMM_MAP_WRITABLE;
    }

    if (page->permissions & PROCESS_IMAGE_PAGE_EXECUTE) {
        vmm_flags |= VMM_MAP_EXECUTABLE;
    }

    /*
     * VMM owns the page-table changes.
     * The process manager continues to own the data frame.
     */
    vmm_status_t status =
        vmm_address_space_map_page(
            process->address_space,
            page->virtual_address,
            page->physical_address,
            vmm_flags
        );

    /*
     * Update ownership state only after the PTE has
     * successfully been installed.
     */
    if (status == VMM_STATUS_OK) {
        page->mapped = true;
    }

    process_lock_release(flags);

    return status == VMM_STATUS_OK;
}

/*
 * Finish construction of the process image.
 *
 * A successful finish verifies that all attached pages
 * are mapped and still owned by the process.
 */
bool process_image_load_finish(
    process_t *process,
    bool success
) {
    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    if (!process->image_loading ||
        process->reference_count == 0 ||
        !process->registered) {

        process_lock_release(flags);
        return false;
    }

    bool valid_image = process->owned_page_sets != NULL;

    for (process_owned_page_set_t *set =
             process->owned_page_sets;
         set != NULL;
         set = set->next) {

        if (!set->attached ||
            set->cleanup_in_progress ||
            set->page_count == 0) {

            valid_image = false;
            break;
        }

        for (uint64_t i = 0;
             i < set->page_count;
             i++) {

            const process_owned_page_t *page =
                &set->pages[i];

            if (!page->frame_owned ||
                !page->mapped ||
                page->quarantined ||
                page->unexpected_physical_address != 0) {

                valid_image = false;
                break;
            }
        }

        if (!valid_image) {
            break;
        }
    }

    if (success && valid_image) {
        process->image_loaded = true;
        process->image_load_failed = false;
    } else {
        process->image_loaded = false;

        /*
         * A failed load with retained ownership records
         * cannot be attempted again on this process.
         *
         * The process may still be destroyed so that its
         * cleanup path can retry releasing those records.
         */
        process->image_load_failed =
            process->owned_page_sets != NULL;
    }

    process->image_loading = false;

    process_lock_release(flags);

    return !success || valid_image;
}

bool process_image_is_loaded(process_t *process) {

    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool result =
        process->registered &&
        process->reference_count > 0 &&
        process->type == PROCESS_TYPE_USER &&
        process->image_loaded &&
        !process->image_loading &&
        !process->image_load_failed &&
        !process->cleanup_in_progress;

    process_lock_release(flags);

    return result;
}


bool process_user_start_begin(
    process_t *process,
    vmm_address_space_t **address_space_out
) {

    if (!process_manager_initialized ||
        process == NULL ||
        address_space_out == NULL) {

        return false;
    }

    *address_space_out = NULL;

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->reference_count > 0 &&
        process->type == PROCESS_TYPE_USER &&
        process->owns_address_space &&
        process->address_space != NULL &&
        process->state == PROCESS_STATE_NEW &&
        process->image_loaded &&
        !process->image_loading &&
        !process->image_load_failed &&
        process->thread_count == 0 &&
        process->thread_head == NULL &&
        !process->user_start_in_progress &&
        !process->user_start_ready &&
        !process->user_start_failed &&
        !process->user_started &&
        !process->cleanup_in_progress;

    if (valid) {

        process->user_start_in_progress = true;

        process->user_stack_base = 0;
        process->user_stack_top = 0;

        *address_space_out = process->address_space;
    }

    process_lock_release(flags);

    return valid;
}


/*
 * Verify every page in a process-owned stack range.
 *
 * The process lock must be held.
 */
static bool process_user_stack_range_valid_unlocked(
    process_t *process,
    uint64_t stack_base,
    uint64_t stack_top
) {

    const uint64_t user_limit =
        0x0000800000000000ULL;

    if (process == NULL ||
        stack_base < 0x10000ULL ||
        stack_top <= stack_base ||
        stack_top > user_limit ||
        (stack_base & 0xFFFULL) != 0 ||
        (stack_top & 0xFFFULL) != 0 ||
        stack_base < PMM_PAGE_SIZE) {

        return false;
    }

    /*
     * The page immediately below the stack must remain unmapped.
     */
    vmm_mapping_info_t guard_mapping = {0};

    if (vmm_address_space_query(
            process->address_space,
            stack_base - PMM_PAGE_SIZE,
            &guard_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return false;
    }

    /*
     * Every owned page set must be internally consistent.
     */
    bool any_owned_pages = false;

    for (process_owned_page_set_t *set =
             process->owned_page_sets;
         set != NULL;
         set = set->next) {

        if (!set->attached ||
            set->cleanup_in_progress ||
            set->page_count == 0) {

            return false;
        }

        for (uint64_t i = 0;
             i < set->page_count;
             i++) {

            const process_owned_page_t *page =
                &set->pages[i];

            if (!page->frame_owned ||
                !page->mapped ||
                page->quarantined ||
                page->unexpected_physical_address != 0) {

                return false;
            }

            any_owned_pages = true;
        }
    }

    if (!any_owned_pages) {
        return false;
    }

    /*
     * Each stack virtual page must have exactly one matching
     * process-owned page record.
     *
     * Also verify the actual page table, not only the bookkeeping.
     */
    for (uint64_t address = stack_base;
         address < stack_top;
         address += PMM_PAGE_SIZE) {

        uint64_t expected_physical = 0;
        uint32_t matches = 0;

        for (process_owned_page_set_t *set =
                 process->owned_page_sets;
             set != NULL;
             set = set->next) {

            for (uint64_t i = 0;
                 i < set->page_count;
                 i++) {

                const process_owned_page_t *page =
                    &set->pages[i];

                if (page->virtual_address != address) {
                    continue;
                }

                if (!page->frame_owned ||
                    !page->mapped ||
                    page->quarantined ||
                    page->permissions !=
                        (PROCESS_IMAGE_PAGE_READ |
                         PROCESS_IMAGE_PAGE_WRITE)) {

                    return false;
                }

                if (matches == 0) {
                    expected_physical =
                        page->physical_address;
                }

                matches++;
            }
        }

        if (matches != 1) {
            return false;
        }

        vmm_mapping_info_t mapping = {0};

        if (vmm_address_space_query(
                process->address_space,
                address,
                &mapping
            ) != VMM_STATUS_OK ||
            mapping.page_size != PMM_PAGE_SIZE ||
            mapping.huge_page ||
            mapping.physical_address != expected_physical ||
            !mapping.user_accessible ||
            !mapping.writable ||
            mapping.executable) {

            return false;
        }
    }

    return true;
}


bool process_user_start_finish(
    process_t *process,
    bool success,
    uint64_t stack_base,
    uint64_t stack_top
) {

    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    if (!process->registered ||
        process->reference_count == 0 ||
        process->type != PROCESS_TYPE_USER ||
        process->state != PROCESS_STATE_NEW ||
        !process->image_loaded ||
        !process->user_start_in_progress ||
        process->thread_count != 0 ||
        process->thread_head != NULL ||
        process->cleanup_in_progress) {

        process_lock_release(flags);
        return false;
    }

    if (!success) {

        /*
         * The caller is aborting the transaction.
         *
         * Keep the failed range so reset_failed() can ensure
         * that no process-owned stack pages remain.
         */
        process->user_start_in_progress = false;
        process->user_start_ready = false;
        process->user_start_failed = true;

        process->user_stack_base = stack_base;
        process->user_stack_top = stack_top;

        process_lock_release(flags);

        return true;
    }

    /*
     * A successful commit requires the complete stack and
     * every process-owned page set to be valid.
     *
     * On validation failure, leave the reservation active.
     * The caller can still invoke finish(false) and roll back.
     */
    bool valid =
        process_user_stack_range_valid_unlocked(
            process,
            stack_base,
            stack_top
        );

    if (!valid) {

        process_lock_release(flags);
        return false;
    }

    process->user_stack_base = stack_base;
    process->user_stack_top = stack_top;

    process->user_start_in_progress = false;
    process->user_start_ready = true;
    process->user_start_failed = false;

    process_lock_release(flags);

    return true;
}


bool process_user_start_reset_failed(process_t *process) {

    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    if (!process->registered ||
        process->reference_count == 0 ||
        process->state != PROCESS_STATE_NEW ||
        !process->image_loaded ||
        process->user_start_in_progress ||
        !process->user_start_failed ||
        process->user_start_ready ||
        process->user_started ||
        process->thread_count != 0 ||
        process->thread_head != NULL ||
        process->cleanup_in_progress ||
        process->user_stack_top <= process->user_stack_base) {

        process_lock_release(flags);
        return false;
    }

    /*
     * A failed stack transaction can be reset only after every
     * process-owned record in that virtual range has disappeared.
     *
     * This intentionally does not reject unrelated pre-existing
     * mappings. A later retry will perform the authoritative
     * virtual-address collision check again.
     */
    for (process_owned_page_set_t *set =
             process->owned_page_sets;
         set != NULL;
         set = set->next) {

        for (uint64_t i = 0;
             i < set->page_count;
             i++) {

            uint64_t address =
                set->pages[i].virtual_address;

            if (address >= process->user_stack_base &&
                address < process->user_stack_top) {

                process_lock_release(flags);
                return false;
            }
        }
    }

    process->user_start_failed = false;

    process->user_stack_base = 0;
    process->user_stack_top = 0;

    process_lock_release(flags);

    return true;
}


bool process_get_user_stack(
    process_t *process,
    uint64_t *stack_base_out,
    uint64_t *stack_top_out
) {

    if (!process_manager_initialized ||
        process == NULL ||
        stack_base_out == NULL ||
        stack_top_out == NULL) {

        return false;
    }

    *stack_base_out = 0;
    *stack_top_out = 0;

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->reference_count > 0 &&
        process->type == PROCESS_TYPE_USER &&
        process->image_loaded &&
        (process->user_start_ready ||
         process->user_started) &&
        !process->user_start_in_progress &&
        !process->user_start_failed &&
        !process->cleanup_in_progress &&
        process->user_stack_top >
            process->user_stack_base;

    if (valid) {
        *stack_base_out = process->user_stack_base;
        *stack_top_out = process->user_stack_top;
    }

    process_lock_release(flags);

    return valid;
}

/*
 * Release one process-owned page set.
 *
 * Ordering is critical:
 *
 *     1. Unmap the virtual page.
 *     2. Verify the returned physical frame.
 *     3. Free the physical frame.
 *     4. Remove the ownership record.
 *
 * A frame is never freed while its mapping may still exist.
 */
static bool process_owned_page_set_release_internal(
    process_t *process,
    process_owned_page_set_t *page_set
) {
    if (!process_manager_initialized ||
        process == NULL ||
        page_set == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool found = false;

    for (process_owned_page_set_t *current =
             process->owned_page_sets;
         current != NULL;
         current = current->next) {

        if (current == page_set) {
            found = true;
            break;
        }
    }

    if (!found ||
        !page_set->attached ||
        page_set->cleanup_in_progress ||
        process->image_loading ||
        process->reference_count == 0 ||
        process->address_space == NULL) {

        process_lock_release(flags);
        return false;
    }

    page_set->cleanup_in_progress = true;

    vmm_address_space_t *address_space =
        process->address_space;

    process_lock_release(flags);

    /*
    * Release each page only after checking the actual page table.
    *
    * The software mapped flag is bookkeeping, not authoritative
    * evidence that the virtual address is currently unmapped.
    */
    for (uint64_t i = 0;
        i < page_set->page_count;
        i++) {

        process_owned_page_t *page =
            &page_set->pages[i];

        if (!page->frame_owned || page->quarantined) {
            continue;
        }

        vmm_mapping_info_t mapping = {0};

        vmm_status_t query_status =
            vmm_address_space_query(
                address_space,
                page->virtual_address,
                &mapping
            );

        if (query_status == VMM_STATUS_NOT_MAPPED) {

            /*
            * The actual page table confirms that this virtual
            * address has no mapping. The frame can be released.
            */
            page->mapped = false;

        } else if (query_status == VMM_STATUS_OK) {

            /*
            * The page table contains a mapping at this address.
            * It must identify our exact frame before we unmap it.
            */
            if (mapping.physical_address !=
                page->physical_address) {

                /*
                * Ownership bookkeeping conflicts with the
                * actual mapping. Never unmap or free based on
                * this inconsistent information.
                */
                page->mapped = false;
                page->quarantined = true;
                page->unexpected_physical_address =
                    mapping.physical_address;

                continue;
            }

            /*
            * A huge mapping cannot be removed through the
            * individual 4 KiB unmap API. Retain ownership and
            * retry only when a suitable operation is available.
            */
            if (mapping.page_size != 4096ULL) {
                page->mapped = true;
                continue;
            }

            page->mapped = true;

            uint64_t unmapped_physical = 0;

            vmm_status_t unmap_status =
                vmm_address_space_unmap_page(
                    address_space,
                    page->virtual_address,
                    &unmapped_physical
                );

            if (unmap_status == VMM_STATUS_OK) {

                page->mapped = false;

                if (unmapped_physical !=
                    page->physical_address) {

                    page->quarantined = true;
                    page->unexpected_physical_address =
                        unmapped_physical;

                    continue;
                }

            } else if (unmap_status == VMM_STATUS_NOT_MAPPED) {

                /*
                * Reconcile an unexpected not-mapped result
                * before deciding that the frame can be freed.
                */
                vmm_mapping_info_t retry_mapping = {0};

                vmm_status_t retry_status =
                    vmm_address_space_query(
                        address_space,
                        page->virtual_address,
                        &retry_mapping
                    );

                if (retry_status == VMM_STATUS_NOT_MAPPED) {

                    page->mapped = false;

                } else if (retry_status == VMM_STATUS_OK) {

                    if (retry_mapping.physical_address ==
                        page->physical_address) {

                        page->mapped = true;

                    } else {

                        page->mapped = false;
                        page->quarantined = true;
                        page->unexpected_physical_address =
                            retry_mapping.physical_address;
                    }

                } else {

                    continue;
                }

            } else {

                /*
                * The unmap operation failed. Preserve the
                * ownership record and retry during later cleanup.
                */
                continue;
            }

        } else {

            /*
            * An invalid or inaccessible page-table state is
            * not evidence that the frame is safe to free.
            */
            continue;
        }

        /*
        * A physical frame may be freed only after the actual
        * page table confirms that its virtual address is unmapped.
        */
        if (!page->mapped &&
            !page->quarantined &&
            page->frame_owned) {

            if (pmm_free_frame(page->physical_address)) {
                page->frame_owned = false;
            }
        }
    }

    /*
     * The metadata can be destroyed only if every frame
     * has been released and no page is quarantined.
     */
    bool complete = true;

    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        const process_owned_page_t *page =
            &page_set->pages[i];

        if (page->frame_owned ||
            page->mapped ||
            page->quarantined) {

            complete = false;
            break;
        }
    }

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    process_owned_page_set_t **link =
        &process->owned_page_sets;

    while (*link != NULL &&
           *link != page_set) {

        link = &(*link)->next;
    }

    if (!complete ||
        *link != page_set ||
        !page_set->attached) {

        page_set->cleanup_in_progress = false;

        process_lock_release(flags);
        return false;
    }

    /*
     * All physical frames are released.
     * Detach the metadata from the process.
     */
    *link = page_set->next;

    page_set->next = NULL;
    page_set->attached = false;
    page_set->cleanup_in_progress = false;

    process_lock_release(flags);

    kfree(page_set);

    return true;
}


/*
 * Public wrapper for releasing a process-owned page set.
 */
bool process_release_owned_pages(
    process_t *process,
    process_owned_page_set_t *page_set
) {
    return process_owned_page_set_release_internal(
        process,
        page_set
    );
}


/*
 * Release all process-owned page sets.
 *
 * Called during final process cleanup, outside process_lock.
 */
static bool process_release_all_owned_pages(
    process_t *process
) {
    for (;;) {

        uint64_t flags = 0;

        if (!process_lock_acquire(&flags)) {
            return false;
        }

        process_owned_page_set_t *page_set =
            process->owned_page_sets;

        process_lock_release(flags);

        if (page_set == NULL) {
            return true;
        }

        if (!process_owned_page_set_release_internal(
                process,
                page_set
            )) {

            return false;
        }
    }
}

/*
 * Release a process reference.
 *
 * The final reference is retained until:
 *   - Every process-owned data frame has been released.
 *   - The private VMM address space has been released.
 *
 * If cleanup fails, the final reference remains at one.
 * Calling process_put() again retries cleanup.
 */
void process_put(process_t *process) {

    if (!process_manager_initialized ||
        process == NULL) {

        return;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return;
    }

    if (process->reference_count == 0) {
        process_lock_release(flags);
        return;
    }

    /*
     * Ordinary reference release.
     */
    if (process->reference_count > 1) {

        process->reference_count--;

        process_lock_release(flags);
        return;
    }

    /*
     * The final reference cannot be released while the
     * process remains registered, owns threads, or is
     * currently being modified by the image loader.
     */
    if (process->user_start_in_progress ||
        process->registered ||
        process->thread_count != 0 ||
        process->thread_head != NULL ||
        process->image_loading ||
        process->cleanup_in_progress) {

        process_lock_release(flags);
        return;
    }

    /*
     * Keep reference_count == 1 during cleanup.
     *
     * This retained reference protects the descriptor if
     * any resource cannot be safely released.
     */
    process->cleanup_in_progress = true;

    vmm_address_space_t *address_space =
        process->address_space;

    bool owns_address_space =
        process->owns_address_space;

    process_lock_release(flags);

    /*
     * PHASE 1:
     *
     * Unmap and release all process-owned data frames.
     *
     * This must happen before page-table destruction.
     */
    bool cleanup_success =
        process_release_all_owned_pages(process);

    /*
     * PHASE 2:
     *
     * Release the private page-table hierarchy.
     *
     * VMM does not free mapped data frames automatically.
     */
    if (cleanup_success && owns_address_space) {

        if (address_space == NULL ||
            !vmm_address_space_put(address_space)) {

            cleanup_success = false;

        } else {

            /*
             * cleanup_in_progress prevents ordinary process
             * APIs from exposing the address-space pointer.
             */
            process->address_space = NULL;
            process->owns_address_space = false;
        }
    }

    /*
     * Do not destroy the descriptor if any resource remains.
     *
     * The final reference is retained so a subsequent
     * process_put() can retry the cleanup.
     */
    if (!cleanup_success) {

        if (process_lock_acquire(&flags)) {

            process->cleanup_in_progress = false;

            process_lock_release(flags);
        }

        return;
    }

    /*
     * PHASE 3:
     *
     * Release the final process reference.
     */
    if (!process_lock_acquire(&flags)) {
        return;
    }

    bool final_state_valid =
        process->reference_count == 1 &&
        !process->registered &&
        process->thread_count == 0 &&
        process->thread_head == NULL &&
        process->owned_page_sets == NULL;

    if (!final_state_valid) {

        process->cleanup_in_progress = false;

        process_lock_release(flags);
        return;
    }

    process->reference_count = 0;
    process->cleanup_in_progress = false;

    process_lock_release(flags);

    kfree(process);
}

/*
 * Mark a process as running.
 */
bool process_set_running(process_t *process) {

    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool result = false;

    if (process->registered &&
        process->state == PROCESS_STATE_NEW &&
        !process->image_loaded &&
        !process->user_start_in_progress &&
        !process->image_loading &&
        !process->image_load_failed &&
        !process->cleanup_in_progress) {

        process->state = PROCESS_STATE_RUNNING;
        result = true;
    }

    process_lock_release(flags);

    return result;
}


/*
 * Begin process termination.
 */
bool process_begin_exit(process_t *process) {

    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool result = false;

    if (process->registered &&
        process->state == PROCESS_STATE_RUNNING) {

        process->state = PROCESS_STATE_EXITING;

        if (process->thread_count == 0) {
            process->state = PROCESS_STATE_ZOMBIE;
        }

        result = true;
    }

    process_lock_release(flags);

    return result;
}

/*
 * Atomically authorize process termination by its sole thread.
 *
 * This deliberately rejects multithreaded processes.
 *
 * HUA does not yet implement coordinated termination of sibling
 * threads, so changing a multithreaded process to EXITING while
 * leaving siblings running would violate its lifecycle rules.
 */
bool process_begin_exit_from_thread(
    process_t *process,
    struct thread *thread
) {

    if (!process_manager_initialized ||
        process == NULL ||
        thread == NULL ||

        /*
        * Exit authorization must begin outside an existing
        * scheduler-managed preemption-disabled critical section.
        *
        * process_lock is a raw IRQ-save spinlock and does not
        * increment the current thread's preemption counter.
        */
        thread->preempt_count != 0) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->type == PROCESS_TYPE_USER &&
        process->state == PROCESS_STATE_RUNNING &&

        process->thread_count == 1U &&
        process->thread_head == thread &&

        process->address_space != NULL &&

        thread->process == process &&
        thread->address_space == process->address_space &&
        thread->state == THREAD_STATE_RUNNING &&

        thread->scheduling_class != THREAD_CLASS_IDLE &&

        thread->run_queue_owner == NULL &&
        thread->run_previous == NULL &&
        thread->run_next == NULL &&

        thread->wait_channel == NULL &&
        thread->wait_previous == NULL &&
        thread->wait_next == NULL &&

        !thread->sleep_queued &&
        thread->sleep_previous == NULL &&
        thread->sleep_next == NULL &&
        thread->sleep_deadline_ns == 0 &&

        !thread->reap_pending &&
        !thread->reap_queued &&
        thread->reap_next == NULL &&

        process_thread_membership_valid_unlocked(
            process,
            thread
        );

    if (valid) {
        process->state = PROCESS_STATE_EXITING;
    }

    process_lock_release(flags);

    return valid;
}

/*
 * Attach one NEW thread to a process.
 */
bool process_attach_thread(
    process_t *process,
    struct thread *thread
) {

    if (!process_manager_initialized ||
        process == NULL ||
        thread == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool image_start_eligible =
        !process->image_loaded ||
        (process->user_start_ready &&
         !process->user_start_in_progress &&
         !process->user_start_failed &&
         !process->user_started &&
         process->thread_count == 0);

    bool valid =
        !process->image_loading &&
        !process->image_load_failed &&
        !process->cleanup_in_progress &&
        process->registered &&
        (process->state == PROCESS_STATE_NEW ||
         process->state == PROCESS_STATE_RUNNING) &&
        process->address_space != NULL &&
        process->thread_count < UINT32_MAX &&
        process->reference_count < UINT64_MAX &&
        !process->user_start_in_progress &&

        image_start_eligible &&

        thread->state == THREAD_STATE_NEW &&
        thread->process == NULL &&
        thread->address_space == NULL &&
        thread->process_previous == NULL &&
        thread->process_next == NULL &&

        thread->run_queue_owner == NULL &&
        thread->run_previous == NULL &&
        thread->run_next == NULL &&

        thread->wait_channel == NULL &&
        thread->wait_previous == NULL &&
        thread->wait_next == NULL &&

        !thread->sleep_queued &&
        thread->sleep_previous == NULL &&
        thread->sleep_next == NULL &&
        thread->sleep_deadline_ns == 0 &&

        !thread->reap_pending &&
        !thread->reap_queued &&
        thread->reap_next == NULL;

    /*
     * The current list head must be internally consistent.
     */
    if (valid &&
        process->thread_head != NULL) {

        if (process->thread_head->process != process ||
            process->thread_head->address_space !=
                process->address_space ||
            process->thread_head->process_previous != NULL) {

            valid = false;
        }
    }

    /*
     * The first thread consumes the committed startup permission.
     */
    if (process->image_loaded) {
        process->user_start_ready = false;
        process->user_started = true;
    }

    if (!valid) {

        process_lock_release(flags);
        return false;
    }

    /*
     * Link at the head.
     */
    thread->process = process;
    thread->address_space = process->address_space;

    thread->process_previous = NULL;
    thread->process_next = process->thread_head;

    if (process->thread_head != NULL) {
        process->thread_head->process_previous = thread;
    }

    process->thread_head = thread;

    /*
     * Acquire the thread's process reference and account
     * for its attachment.
     */
    process->thread_count++;
    process->reference_count++;

    process->state = PROCESS_STATE_RUNNING;

    process_lock_release(flags);

    return true;
}

bool process_cancel_initial_thread(
    process_t *process,
    struct thread *thread
) {

    if (!process_manager_initialized ||
        process == NULL ||
        thread == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->type == PROCESS_TYPE_USER &&
        process->image_loaded &&
        process->state == PROCESS_STATE_RUNNING &&
        process->thread_count == 1U &&
        process->thread_head == thread &&
        process->reference_count > 1 &&
        process->user_started &&
        !process->user_start_ready &&
        !process->user_start_in_progress &&
        !process->user_start_failed &&

        thread->state == THREAD_STATE_NEW &&
        thread->run_queue_owner == NULL &&
        thread->run_previous == NULL &&
        thread->run_next == NULL &&
        thread->wait_channel == NULL &&
        thread->wait_previous == NULL &&
        thread->wait_next == NULL &&
        !thread->sleep_queued &&
        thread->sleep_previous == NULL &&
        thread->sleep_next == NULL &&
        thread->sleep_deadline_ns == 0 &&
        !thread->reap_pending &&
        !thread->reap_queued &&
        thread->reap_next == NULL &&

        process_thread_membership_valid_unlocked(
            process,
            thread
        );

    if (!valid) {

        process_lock_release(flags);
        return false;
    }

    process_thread_unlink_unlocked(
        process,
        thread
    );

    process->thread_count--;
    process->reference_count--;

    /*
     * Restore the original NEW, image-loaded state so that the
     * caller can retry the enqueue operation.
     */
    process->state = PROCESS_STATE_NEW;

    process->user_started = false;
    process->user_start_ready = true;

    process_lock_release(flags);

    return true;
}

/*
 * Detach a process-owned NEW thread.
 *
 * This path is for a thread that never became runnable.
 */
bool process_detach_thread(
    process_t *process,
    struct thread *thread
) {

    if (!process_manager_initialized ||
        process == NULL ||
        thread == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    if (!process->registered ||
        thread->state != THREAD_STATE_NEW ||
        process->thread_count == 0 ||
        process->reference_count <= 1 ||
        !process_thread_membership_valid_unlocked(
            process,
            thread
        )) {

        process_lock_release(flags);
        return false;
    }

    /*
     * Remove the actual thread before decrementing accounting.
     */
    process_thread_unlink_unlocked(
        process,
        thread
    );

    process->thread_count--;
    process->reference_count--;

    /*
     * An exiting process becomes a zombie when its final
     * attached thread is detached.
     *
     * A normal RUNNING process is not implicitly terminated.
     */
    if (process->thread_count == 0 &&
        process->state == PROCESS_STATE_EXITING) {

        process->state = PROCESS_STATE_ZOMBIE;
    }

    process_lock_release(flags);

    return true;
}


/*
 * Account for a permanently terminated thread.
 *
 * This is called by the deferred scheduler reaper, not
 * from the thread's own execution stack or an interrupt.
 */
bool process_thread_exit(
    process_t *process,
    struct thread *thread
) {

    if (!process_manager_initialized ||
        process == NULL ||
        thread == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    if (!process->registered ||
        thread->state != THREAD_STATE_TERMINATED ||
        process->thread_count == 0 ||
        process->reference_count <= 1 ||
        (process->state != PROCESS_STATE_RUNNING &&
         process->state != PROCESS_STATE_EXITING) ||
        !process_thread_membership_valid_unlocked(
            process,
            thread
        )) {

        process_lock_release(flags);
        return false;
    }

    /*
     * If this is the final attached thread, preserve its
     * exit status before unlinking its TCB.
     *
     * A non-final thread's exit status remains an individual
     * thread result; it must not overwrite the process's
     * eventual termination status.
     */
    if (process->thread_count == 1U) {

        process->exit_code = thread->exit_code;
    }

    /*
     * Remove the thread and release its process ownership.
     */
    process_thread_unlink_unlocked(
        process,
        thread
    );

    process->thread_count--;
    process->reference_count--;

    /*
     * The final terminated thread makes the process a zombie.
     */
    if (process->thread_count == 0) {

        process->state = PROCESS_STATE_ZOMBIE;
    }

    process_lock_release(flags);

    return true;
}

/*
 * Record an unhandled user-mode exception.
 *
 * This operation is atomic with respect to process lifecycle
 * changes because both the exception record and process state
 * are protected by process_lock.
 */
bool process_record_user_exception(
    process_t *process,
    struct thread *thread,
    const process_exception_info_t *information
) {

    if (!process_manager_initialized ||
        process == NULL ||
        thread == NULL ||
        information == NULL ||
        information->vector >= 32) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->type == PROCESS_TYPE_USER &&
        process->state == PROCESS_STATE_RUNNING &&

        /*
         * The first implementation only supports a
         * single-threaded process.
         */
        process->thread_count == 1 &&
        process->thread_head == thread &&

        thread->state == THREAD_STATE_RUNNING &&
        thread->id == information->thread_id &&

        thread->process_previous == NULL &&
        thread->process_next == NULL &&

        !process->exception_info.valid &&

        process_thread_membership_valid_unlocked(
            process,
            thread
        );

    if (valid) {

        /*
         * Preserve the exception before the thread can
         * be detached and destroyed by the reaper.
         */
        process->exception_info = *information;
        process->exception_info.valid = true;

        /*
         * Prevent future thread attachment and mark the
         * process as being terminated.
         *
         * process_thread_exit() accepts EXITING and will
         * transition the final thread's process to ZOMBIE.
         */
        process->state = PROCESS_STATE_EXITING;
    }

    process_lock_release(flags);

    return valid;
}


/*
 * Retrieve a previously recorded user-mode exception.
 */
bool process_get_exception_info(
    process_t *process,
    process_exception_info_t *information_out
) {

    if (!process_manager_initialized ||
        process == NULL ||
        information_out == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->exception_info.valid;

    if (valid) {
        *information_out = process->exception_info;
    }

    process_lock_release(flags);

    return valid;
}

/*
 * Remove a process from the global registry.
 */
bool process_destroy(process_t *process) {

    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    if (!process->registered ||
        process->thread_count != 0 ||
        process->thread_head != NULL ||
        process->reference_count <= 1 ||
        process->image_loading ||
        process->user_start_in_progress ||
        process->cleanup_in_progress ||
        (process->state != PROCESS_STATE_NEW &&
        process->state != PROCESS_STATE_ZOMBIE)) {

        process_lock_release(flags);
        return false;
    }

    process_t **link = &process_registry;

    while (*link != NULL &&
           *link != process) {

        link = &(*link)->registry_next;
    }

    if (*link != process) {

        process_lock_release(flags);
        return false;
    }

    /*
     * Unlink the process.
     */
    *link = process->registry_next;

    process->registry_next = NULL;

    process->registered = false;

    /*
    * The caller is required to hold its own reference.
    * Therefore, dropping the registry reference must
    * never be the final reference.
    */
    process->reference_count--;

    process->state = PROCESS_STATE_ZOMBIE;

    process_lock_release(flags);

    return true;
}


/*
 * Read process information safely.
 *
 * name_capacity must be at least PROCESS_NAME_MAX
 * when name is requested.
 */
bool process_get_info(
    process_t *process,
    process_id_t *pid,
    process_state_t *state,
    uint32_t *thread_count,
    char *name,
    uint32_t name_capacity
) {

    if (!process_manager_initialized ||
        process == NULL) {

        return false;
    }

    if (name != NULL &&
        name_capacity < PROCESS_NAME_MAX) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid = process->registered;

    if (valid) {

        if (pid != NULL) {
            *pid = process->pid;
        }

        if (state != NULL) {
            *state = process->state;
        }

        if (thread_count != NULL) {
            *thread_count = process->thread_count;
        }

        if (name != NULL) {

            for (uint32_t i = 0;
                 i < PROCESS_NAME_MAX;
                 i++) {

                name[i] = process->name[i];
            }
        }
    }

    process_lock_release(flags);

    return valid;
}

/*
 * Retrieve a durable process exit status.
 *
 * The status becomes available only after the final attached
 * thread has been detached by the scheduler reaper.
 *
 * The caller must hold a valid process reference.
 */
bool process_get_exit_code(
    process_t *process,
    int64_t *exit_code_out
) {

    if (!process_manager_initialized ||
        process == NULL ||
        exit_code_out == NULL) {

        return false;
    }

    uint64_t flags = 0;

    if (!process_lock_acquire(&flags)) {
        return false;
    }

    bool valid =
        process->registered &&
        process->state == PROCESS_STATE_ZOMBIE &&
        process->thread_count == 0 &&
        process->thread_head == NULL;

    if (valid) {
        *exit_code_out = process->exit_code;
    }

    process_lock_release(flags);

    return valid;
}
