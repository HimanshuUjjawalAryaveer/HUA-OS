#include "../process/process.h"
#include "../scheduler/thread.h"
#include "drivers/console/console.h"
#include <stddef.h>
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"

#include "process_tests.h"

static void process_test_thread_entry(void *argument);

static bool process_test_create_and_lookup(void)
{
    process_t *process = process_create_kernel("process-test");

    if (process == NULL) {
        return false;
    }

    /*
    * A kernel process must use the permanent kernel
    * address space.
    */
    vmm_address_space_t *address_space = NULL;

    bool address_space_ok =
        process_get_address_space(
            process,
            &address_space
        ) &&
        address_space == vmm_kernel_address_space();

    process_id_t pid = 0;
    process_state_t state = PROCESS_STATE_ZOMBIE;
    uint32_t thread_count = 0;
    char name[PROCESS_NAME_MAX];

    bool info_ok = process_get_info(
        process,
        &pid,
        &state,
        &thread_count,
        name,
        sizeof(name)
    );

    process_t *lookup = process_get_by_pid(pid);

    bool passed =
        info_ok &&
        address_space_ok &&
        pid != 0 &&
        state == PROCESS_STATE_NEW &&
        thread_count == 0 &&
        lookup == process;

    if (lookup != NULL) {
        process_put(lookup);
    }

    if (!process_destroy(process)) {
        process_put(process);
        return false;
    }

    process_put(process);

    return passed;
}

static bool process_test_thread_lifecycle(void)
{
    process_t *process =
        process_create_kernel("thread-test");

    if (process == NULL) {
        return false;
    }

    struct thread *thread = thread_create(
        "process-lifecycle-worker",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (thread == NULL) {
        (void)process_destroy(process);
        process_put(process);
        return false;
    }

    bool attached =
        process_attach_thread(process, thread);

    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = 0;

    bool info_ok = process_get_info(
        process,
        NULL,
        &state,
        &thread_count,
        NULL,
        0
    );

    bool attach_ok =
        attached &&
        info_ok &&
        thread->process == process &&
        thread->address_space ==
            vmm_kernel_address_space() &&
        state == PROCESS_STATE_RUNNING &&
        thread_count == 1;

    bool exit_started =
        process_begin_exit(process);

    bool detached =
        process_detach_thread(process, thread);

    bool thread_destroyed =
        thread_destroy(thread);

    info_ok = process_get_info(
        process,
        NULL,
        &state,
        &thread_count,
        NULL,
        0
    );

    bool lifecycle_ok =
        exit_started &&
        detached &&
        thread_destroyed &&
        info_ok &&
        state == PROCESS_STATE_ZOMBIE &&
        thread_count == 0;

    bool destroyed =
        process_destroy(process);

    process_put(process);

    return attach_ok &&
           lifecycle_ok &&
           destroyed;
}

static bool process_test_multiple_threads(void)
{
    process_t *process =
        process_create_kernel("multi-thread-test");

    if (process == NULL) {
        return false;
    }

    struct thread *first_thread = thread_create(
        "process-multi-first",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    struct thread *second_thread = thread_create(
        "process-multi-second",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (first_thread == NULL ||
        second_thread == NULL) {

        if (first_thread != NULL) {
            (void)thread_destroy(first_thread);
        }

        if (second_thread != NULL) {
            (void)thread_destroy(second_thread);
        }

        (void)process_destroy(process);
        process_put(process);
        return false;
    }

    bool first =
        process_attach_thread(process, first_thread);

    bool second =
        process_attach_thread(process, second_thread);

    bool exit_started =
        first && second &&
        process_begin_exit(process);

    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = 0;

    bool initial_info = process_get_info(
        process,
        NULL,
        &state,
        &thread_count,
        NULL,
        0
    );

    bool initial_ok =
        initial_info &&
        state == PROCESS_STATE_EXITING &&
        thread_count == 2;

    bool first_detach =
        first &&
        process_detach_thread(process, first_thread);

    bool intermediate_info =
        process_get_info(
            process,
            NULL,
            &state,
            &thread_count,
            NULL,
            0
        );

    bool intermediate_ok =
        intermediate_info &&
        state == PROCESS_STATE_EXITING &&
        thread_count == 1;

    bool first_destroyed =
        first_detach &&
        thread_destroy(first_thread);

    bool second_detach =
        second &&
        process_detach_thread(process, second_thread);

    bool final_info =
        process_get_info(
            process,
            NULL,
            &state,
            &thread_count,
            NULL,
            0
        );

    bool final_ok =
        final_info &&
        state == PROCESS_STATE_ZOMBIE &&
        thread_count == 0;

    bool second_destroyed =
        second_detach &&
        thread_destroy(second_thread);

    bool destroyed =
        first_destroyed &&
        second_destroyed &&
        process_destroy(process);

    /*
     * If any test operation failed, avoid freeing a process
     * that still has attached threads.
     */
    if (!destroyed) {

        if (first_thread->process == process) {
            (void)process_detach_thread(
                process,
                first_thread
            );
        }

        if (second_thread->process == process) {
            (void)process_detach_thread(
                process,
                second_thread
            );
        }

        if (first_thread->process == NULL) {
            (void)thread_destroy(first_thread);
        }

        if (second_thread->process == NULL) {
            (void)thread_destroy(second_thread);
        }
    }

    process_put(process);

    return first &&
           second &&
           exit_started &&
           initial_ok &&
           first_detach &&
           intermediate_ok &&
           first_destroyed &&
           second_detach &&
           final_ok &&
           second_destroyed &&
           destroyed;
}

static bool process_test_destroy_with_live_thread(void)
{
    process_t *process =
        process_create_kernel("destroy-test");

    if (process == NULL) {
        return false;
    }

    struct thread *thread = thread_create(
        "process-destroy-worker",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (thread == NULL) {
        (void)process_destroy(process);
        process_put(process);
        return false;
    }

    bool attached =
        process_attach_thread(process, thread);

    /*
     * A process with an attached TCB cannot be destroyed.
     */
    bool rejected =
        !process_destroy(process);

    bool detached =
        attached &&
        process_detach_thread(process, thread);

    bool thread_destroyed =
        detached &&
        thread_destroy(thread);

    /*
     * The process remains RUNNING after an ordinary detach.
     * Explicitly begin its exit before destruction.
     */
    bool exit_started =
        thread_destroyed &&
        process_begin_exit(process);

    bool destroyed =
        exit_started &&
        process_destroy(process);

    process_put(process);

    return attached &&
           rejected &&
           detached &&
           thread_destroyed &&
           exit_started &&
           destroyed;
}

/*
 * Minimal entry function used by the process-thread
 * integration test.
 *
 * The test creates the thread but does not schedule it.
 */
static void process_test_thread_entry(void *argument) {

    (void)argument;
}


/*
 * Verify that process-owned thread creation correctly
 * integrates the process manager and thread subsystem.
 */
static bool process_test_thread_integration(void) {

    process_t *process =
        process_create_kernel("process-thread-test");

    if (process == NULL) {
        return false;
    }

    /*
     * Create a NEW thread owned by this process.
     */
    struct thread *thread = thread_create_for_process(
        process,
        "process-worker",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (thread == NULL) {

        (void)process_begin_exit(process);
        (void)process_destroy(process);
        process_put(process);

        return false;
    }

    /*
     * Check that the thread and process agree about ownership.
     */
    bool ownership_ok =
        thread->process == process &&
        thread->state == THREAD_STATE_NEW;

    process_id_t pid = 0;
    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = 0;

    bool attached_info_ok = process_get_info(
        process,
        &pid,
        &state,
        &thread_count,
        NULL,
        0
    );

    bool attached_ok =
        attached_info_ok &&
        state == PROCESS_STATE_RUNNING &&
        thread_count == 1;

    /*
     * The thread has not run yet.
     *
     * Destroying this NEW thread must detach it from
     * the process and release its process reference.
     */
    bool destroyed = thread_destroy(thread);

    bool detached_info_ok = process_get_info(
        process,
        &pid,
        &state,
        &thread_count,
        NULL,
        0
    );

    bool detached_ok =
        detached_info_ok &&
        state == PROCESS_STATE_RUNNING &&
        thread_count == 0;

    /*
     * Explicitly begin process termination after the
     * unstarted thread has been destroyed.
     */
    bool exit_started = process_begin_exit(process);

    bool zombie_info_ok = process_get_info(
        process,
        &pid,
        &state,
        &thread_count,
        NULL,
        0
    );

    bool zombie_ok =
        zombie_info_ok &&
        state == PROCESS_STATE_ZOMBIE &&
        thread_count == 0;

    bool process_destroyed = process_destroy(process);

    process_put(process);

    return ownership_ok &&
           attached_ok &&
           destroyed &&
           detached_ok &&
           exit_started &&
           zombie_ok &&
           process_destroyed;
}

/*
 * Verify process-owned address-space lifetime and
 * sharing between multiple threads in one process.
 */
static bool process_test_address_space_ownership(void) {

    process_t *process_a = NULL;
    process_t *process_b = NULL;

    struct thread *thread_a1 = NULL;
    struct thread *thread_a2 = NULL;

    vmm_address_space_t *space_a = NULL;
    vmm_address_space_t *space_b = NULL;

    uint64_t root_a = 0;
    uint64_t root_b = 0;
    uint64_t stale_root = 0;

    bool process_a_destroyed = false;
    bool process_b_destroyed = false;

    /*
     * Invalid names must be rejected without creating
     * a process or retaining an address space.
     */
    if (process_create_user(NULL) != NULL ||
        process_create_user("") != NULL) {

        goto failure;
    }

    /*
     * Create two independent user processes.
     */
    process_a = process_create_user("user-process-a");

    if (process_a == NULL) {
        goto failure;
    }

    process_b = process_create_user("user-process-b");

    if (process_b == NULL) {
        goto failure;
    }

    /*
     * Retrieve the address-space associations.
     */
    if (!process_get_address_space(
            process_a,
            &space_a
        ) ||
        !process_get_address_space(
            process_b,
            &space_b
        )) {

        goto failure;
    }

    /*
     * Neither user process may borrow the kernel root.
     * Two different processes must not share private roots.
     */
    if (space_a == NULL ||
        space_b == NULL ||
        space_a == space_b ||
        space_a == vmm_kernel_address_space() ||
        space_b == vmm_kernel_address_space()) {

        goto failure;
    }

    /*
     * Verify physical-root independence.
     */
    if (!vmm_address_space_get_root_physical(
            space_a,
            &root_a
        ) ||
        !vmm_address_space_get_root_physical(
            space_b,
            &root_b
        )) {

        goto failure;
    }

    if (root_a == 0 ||
        root_b == 0 ||
        root_a == root_b ||
        (root_a & 0xFFFULL) != 0 ||
        (root_b & 0xFFFULL) != 0) {

        goto failure;
    }

    /*
     * Empty user-process address spaces must not contain
     * inherited user mappings.
     */
    vmm_mapping_info_t mapping;

    if (vmm_address_space_query(
            space_a,
            0x00400000ULL,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_address_space_query(
            space_b,
            0x00400000ULL,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto failure;
    }

    /*
     * Create two threads belonging to process A.
     *
     * Both threads must refer to the same owning process,
     * and therefore share its address space.
     */
    thread_a1 = thread_create_for_process(
        process_a,
        "user-worker-a1",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (thread_a1 == NULL) {
        goto failure;
    }

    thread_a2 = thread_create_for_process(
        process_a,
        "user-worker-a2",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (thread_a2 == NULL) {
        goto failure;
    }

    vmm_address_space_t *thread_space = NULL;

    if (thread_a1->process != process_a ||
        thread_a2->process != process_a ||
        !process_get_address_space(
            thread_a1->process,
            &thread_space
        ) ||
        thread_space != space_a) {

        goto failure;
    }

    if (!process_get_address_space(
            thread_a2->process,
            &thread_space
        ) ||
        thread_space != space_a) {

        goto failure;
    }

    /*
     * A process with live threads cannot be destroyed.
     */
    if (process_destroy(process_a)) {
        goto failure;
    }

    /*
     * Begin process termination.
     *
     * The two NEW threads will be destroyed without
     * ever being scheduled.
     */
    if (!process_begin_exit(process_a)) {
        goto failure;
    }

    /*
     * Destroying the first thread must leave the process
     * exiting with one remaining thread.
     */
    if (!thread_destroy(thread_a1)) {
        goto failure;
    }

    thread_a1 = NULL;

    process_id_t pid = 0;
    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = 0;

    if (!process_get_info(
            process_a,
            &pid,
            &state,
            &thread_count,
            NULL,
            0
        ) ||
        state != PROCESS_STATE_EXITING ||
        thread_count != 1) {

        goto failure;
    }

    /*
     * Destroying the final thread must transition the
     * exiting process to ZOMBIE.
     */
    if (!thread_destroy(thread_a2)) {
        goto failure;
    }

    thread_a2 = NULL;

    if (!process_get_info(
            process_a,
            &pid,
            &state,
            &thread_count,
            NULL,
            0
        ) ||
        state != PROCESS_STATE_ZOMBIE ||
        thread_count != 0) {

        goto failure;
    }

    /*
     * Now the process can leave the registry.
     */
    if (!process_destroy(process_a)) {
        goto failure;
    }

    process_a_destroyed = true;

    /*
     * Release the caller's final process reference.
     *
     * This must also release its private VMM address space.
     */
    process_put(process_a);
    process_a = NULL;

    /*
     * A destroyed process address space must no longer
     * be accepted by the VMM registry.
     *
     * The stale pointer is only compared against registered
     * objects; the test does not dereference it.
     */
    if (vmm_address_space_get_root_physical(
            space_a,
            &stale_root
        )) {

        goto failure;
    }

    /*
     * Independently destroy process B, which has no threads.
     * Its initial state is NEW, so it can be destroyed directly.
     */
    if (!process_destroy(process_b)) {
        goto failure;
    }

    process_b_destroyed = true;

    process_put(process_b);
    process_b = NULL;

    if (vmm_address_space_get_root_physical(
            space_b,
            &stale_root
        )) {

        goto failure;
    }

    console_write(
        "  [ OK ] Independent process address spaces\n"
    );

    console_write(
        "  [ OK ] Multiple threads share process address space\n"
    );

    console_write(
        "  [ OK ] Address space retained until final process reference\n"
    );

    console_write(
        "  [ OK ] Process destruction releases private address space\n"
    );

    return true;


failure:

    /*
     * Best-effort cleanup.
     *
     * Destroy unstarted threads first so their process
     * attachments and references are released.
     */
    if (thread_a1 != NULL) {
        if (thread_destroy(thread_a1)) {
            thread_a1 = NULL;
        }
    }

    if (thread_a2 != NULL) {
        if (thread_destroy(thread_a2)) {
            thread_a2 = NULL;
        }
    }

    /*
     * Begin exit after detaching all successfully destroyed
     * test threads.
     */
    if (process_a != NULL) {

        (void)process_begin_exit(process_a);

        if (process_destroy(process_a)) {
            process_a_destroyed = true;
        }

        /*
         * Release only the caller's reference.
         * If threads remain attached, their references keep
         * the process and address space alive.
         */
        process_put(process_a);
        process_a = NULL;
    }

    if (process_b != NULL) {

        (void)process_begin_exit(process_b);

        if (process_destroy(process_b)) {
            process_b_destroyed = true;
        }

        process_put(process_b);
        process_b = NULL;
    }

    (void)process_a_destroyed;
    (void)process_b_destroyed;

    return false;
}

/*
 * Verify process-exit authorization.
 *
 * Tests:
 *
 * 1. A process with two attached threads must be rejected.
 * 2. Rejection must leave the process RUNNING.
 * 3. A process with exactly one attached thread is accepted.
 * 4. Its final exit status survives TCB destruction.
 *
 * This is a process-manager unit test. It deliberately
 * constructs the relevant TCB states without scheduling
 * either test thread.
 */
static bool process_test_exit_authorization(void) {

    static const int64_t expected_exit_code =
        -INT64_C(0x123456789);

    process_t *process =
        process_create_user("process-exit-authorization");

    if (process == NULL) {
        return false;
    }

    struct thread *first = NULL;
    struct thread *second = NULL;

    bool passed = false;

    /*
     * Create two process-owned NEW threads.
     *
     * Attaching the first thread transitions the process
     * from NEW to RUNNING.
     */
    first = thread_create_for_process(
        process,
        "process-exit-first",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (first == NULL) {
        goto cleanup;
    }

    second = thread_create_for_process(
        process,
        "process-exit-second",
        process_test_thread_entry,
        NULL,
        THREAD_STACK_DEFAULT_SIZE
    );

    if (second == NULL) {
        goto cleanup;
    }

    /*
     * Simulate the first TCB being the current RUNNING thread.
     *
     * The second thread remains NEW but attached.
     */
    first->state = THREAD_STATE_RUNNING;

    bool multiple_rejected =
        !process_begin_exit_from_thread(
            process,
            first
        );

    /*
     * Restore the TCB to NEW before the unstarted-thread
     * destruction path.
     */
    first->state = THREAD_STATE_NEW;

    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = 0;

    bool information_valid = process_get_info(
        process,
        NULL,
        &state,
        &thread_count,
        NULL,
        0
    );

    bool rejection_preserved_state =
        information_valid &&
        state == PROCESS_STATE_RUNNING &&
        thread_count == 2U;

    if (!multiple_rejected ||
        !rejection_preserved_state) {

        goto cleanup;
    }

    /*
     * Remove the second NEW thread.
     *
     * This leaves the process with exactly one attached thread.
     */
    if (!thread_destroy(second)) {
        goto cleanup;
    }

    second = NULL;

    /*
     * Now the first thread is the sole attached thread.
     * Authorization must succeed.
     */
    first->state = THREAD_STATE_RUNNING;

    bool accepted =
        process_begin_exit_from_thread(
            process,
            first
        );

    if (!accepted) {
        first->state = THREAD_STATE_NEW;
        goto cleanup;
    }

    information_valid = process_get_info(
        process,
        NULL,
        &state,
        &thread_count,
        NULL,
        0
    );

    if (!information_valid ||
        state != PROCESS_STATE_EXITING ||
        thread_count != 1U) {

        first->state = THREAD_STATE_NEW;
        goto cleanup;
    }

    /*
     * Simulate the scheduler's completed thread-exit state
     * immediately before deferred reaper accounting.
     *
     * The real Ring 3 test below verifies this through the
     * actual thread_exit() and scheduler paths.
     */
    first->exit_code = expected_exit_code;
    first->state = THREAD_STATE_TERMINATED;
    first->reap_pending = true;
    first->reap_queued = false;
    first->reap_next = NULL;

    if (!process_thread_exit(process, first)) {
        first->state = THREAD_STATE_NEW;
        first->reap_pending = false;
        goto cleanup;
    }

    /*
     * process_thread_exit() has detached the TCB and
     * copied its exit status into the process.
     */
    if (!thread_destroy(first)) {
        first = NULL;
        goto cleanup;
    }

    first = NULL;

    information_valid = process_get_info(
        process,
        NULL,
        &state,
        &thread_count,
        NULL,
        0
    );

    int64_t observed_exit_code = 0;

    bool exit_code_valid = process_get_exit_code(
        process,
        &observed_exit_code
    );

    passed =
        information_valid &&
        state == PROCESS_STATE_ZOMBIE &&
        thread_count == 0U &&
        exit_code_valid &&
        observed_exit_code == expected_exit_code;

cleanup:

    /*
     * Best-effort cleanup for any remaining unstarted TCBs.
     */
    if (second != NULL) {

        (void)thread_destroy(second);
        second = NULL;
    }

    if (first != NULL) {

        /*
         * If the simulated reaper has not detached this TCB,
         * restore a valid NEW state and let thread_destroy()
         * perform ordinary process detachment.
         */
        if (first->process == process) {

            first->state = THREAD_STATE_NEW;
            first->reap_pending = false;
            first->reap_queued = false;
            first->reap_next = NULL;
        }

        (void)thread_destroy(first);
        first = NULL;
    }

    /*
     * A process with zero threads but still RUNNING needs an
     * explicit termination request before it can be destroyed.
     */
    if (!passed) {

        state = PROCESS_STATE_NEW;
        thread_count = UINT32_MAX;

        if (process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            ) &&
            state == PROCESS_STATE_RUNNING &&
            thread_count == 0U) {

            (void)process_begin_exit(process);
        }
    }

    if (!passed) {

        state = PROCESS_STATE_NEW;
        thread_count = UINT32_MAX;

        if (process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            ) &&
            thread_count == 0U &&
            (state == PROCESS_STATE_NEW ||
             state == PROCESS_STATE_ZOMBIE)) {

            (void)process_destroy(process);
        }
    } else {

        (void)process_destroy(process);
    }

    process_put(process);

    return passed;
}

#define PROCESS_IMAGE_TEST_PAGE_COUNT 2ULL

#define PROCESS_IMAGE_TEST_CODE_VA 0x00400000ULL
#define PROCESS_IMAGE_TEST_DATA_VA 0x00401000ULL

/*
 * Allocate two owned frames and construct a page set.
 *
 * physical_by_virtual[0] always records the code frame.
 * physical_by_virtual[1] always records the data frame.
 *
 * The page-set records are sorted by physical address because
 * process_image_load_attach_pages() requires that ordering.
 */
static process_owned_page_set_t *
process_test_create_image_page_set(
    uint64_t physical_by_virtual[PROCESS_IMAGE_TEST_PAGE_COUNT]
) {
    if (physical_by_virtual == NULL) {
        return NULL;
    }

    physical_by_virtual[0] = 0;
    physical_by_virtual[1] = 0;

    size_t allocation_size =
        sizeof(process_owned_page_set_t) +
        PROCESS_IMAGE_TEST_PAGE_COUNT *
            sizeof(process_owned_page_t);

    process_owned_page_set_t *page_set =
        (process_owned_page_set_t *)kcalloc(
            1,
            allocation_size
        );

    if (page_set == NULL) {
        return NULL;
    }

    page_set->page_count = PROCESS_IMAGE_TEST_PAGE_COUNT;

    for (uint64_t i = 0;
         i < PROCESS_IMAGE_TEST_PAGE_COUNT;
         i++) {

        uint64_t physical_address = pmm_alloc_frame();

        if (physical_address == 0) {

            for (uint64_t j = 0; j < i; j++) {
                (void)pmm_free_frame(
                    physical_by_virtual[j]
                );
            }

            kfree(page_set);
            return NULL;
        }

        physical_by_virtual[i] = physical_address;

        process_owned_page_t *page =
            &page_set->pages[i];

        page->virtual_address =
            i == 0
                ? PROCESS_IMAGE_TEST_CODE_VA
                : PROCESS_IMAGE_TEST_DATA_VA;

        page->physical_address = physical_address;

        page->permissions =
            PROCESS_IMAGE_PAGE_READ |
            (i == 0
                ? PROCESS_IMAGE_PAGE_EXECUTE
                : PROCESS_IMAGE_PAGE_WRITE);

        page->frame_owned = true;
    }

    /*
     * The process API requires strictly increasing physical
     * addresses. Sort the two complete records, not just the
     * physical-address fields.
     */
    if (page_set->pages[0].physical_address >
        page_set->pages[1].physical_address) {

        process_owned_page_t temporary =
            page_set->pages[0];

        page_set->pages[0] =
            page_set->pages[1];

        page_set->pages[1] = temporary;
    }

    return page_set;
}


/*
 * Find a page-set record by virtual address.
 *
 * The page-set is sorted by physical address, not virtual
 * address, so callers must not assume its index from its VA.
 */
static uint64_t process_test_find_image_page(
    const process_owned_page_set_t *page_set,
    uint64_t virtual_address
) {
    if (page_set == NULL) {
        return UINT64_MAX;
    }

    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        if (page_set->pages[i].virtual_address ==
            virtual_address) {

            return i;
        }
    }

    return UINT64_MAX;
}


/*
 * Release a page set that has not been transferred to a process.
 *
 * The caller still owns both the metadata and the frames.
 */
static bool process_test_release_unattached_page_set(
    process_owned_page_set_t *page_set
) {
    if (page_set == NULL ||
        page_set->attached ||
        page_set->cleanup_in_progress) {

        return false;
    }

    bool success = true;

    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        process_owned_page_t *page =
            &page_set->pages[i];

        if (page->frame_owned) {

            if (pmm_free_frame(page->physical_address)) {
                page->frame_owned = false;
            } else {
                success = false;
            }
        }
    }

    /*
     * Never discard ownership metadata while it still
     * describes an allocated frame.
     */
    if (success) {
        kfree(page_set);
    }

    return success;
}


/*
 * Verify that both test addresses are unmapped and that
 * neither physical frame remains allocated by PMM.
 */
static bool process_test_image_pages_released(
    vmm_address_space_t *address_space,
    const uint64_t physical_by_virtual[
        PROCESS_IMAGE_TEST_PAGE_COUNT
    ]
) {
    if (address_space == NULL ||
        physical_by_virtual == NULL) {

        return false;
    }

    vmm_mapping_info_t mapping;

    if (vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_CODE_VA,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return false;
    }

    if (vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return false;
    }

    return
        pmm_frame_to_virt(physical_by_virtual[0]) == NULL &&
        pmm_frame_to_virt(physical_by_virtual[1]) == NULL;
}

/*
 * Verify process-owned image mapping, effective permissions,
 * unmapping, and physical-frame release.
 */
static bool process_test_image_page_mapping(void)
{
    process_t *process =
        process_create_user("image-map-test");

    if (process == NULL) {
        return false;
    }

    vmm_address_space_t *address_space = NULL;

    process_owned_page_set_t *page_set = NULL;

    uint64_t physical_by_virtual[
        PROCESS_IMAGE_TEST_PAGE_COUNT
    ] = {0};

    bool loading = false;
    bool attached = false;
    bool passed = false;
    bool cleanup_ok = true;

    if (!process_image_load_begin(
            process,
            &address_space
        )) {

        goto cleanup;
    }

    loading = true;

    if (address_space == NULL) {
        goto cleanup;
    }

    page_set =
        process_test_create_image_page_set(
            physical_by_virtual
        );

    if (page_set == NULL) {
        goto cleanup;
    }

    /*
     * Ownership transfers to the process only on success.
     */
    if (!process_image_load_attach_pages(
            process,
            page_set
        )) {

        goto cleanup;
    }

    attached = true;

    uint64_t code_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_CODE_VA
        );

    uint64_t data_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_DATA_VA
        );

    if (code_index == UINT64_MAX ||
        data_index == UINT64_MAX) {

        goto cleanup;
    }

    /*
     * Map the code page:
     * user-readable, non-writable, executable.
     */
    if (!process_image_load_map_page(
            process,
            page_set,
            code_index
        )) {

        goto cleanup;
    }

    /*
     * Map the data page:
     * user-readable, writable, non-executable.
     */
    if (!process_image_load_map_page(
            process,
            page_set,
            data_index
        )) {

        goto cleanup;
    }

    /*
     * The image may finish only after all attached pages
     * have been successfully mapped.
     */
    bool finish_ok =
        process_image_load_finish(process, true);

    loading = false;

    if (!finish_ok) {
        goto cleanup;
    }

    /*
     * Inspect the actual page tables rather than relying
     * only on the process ownership records.
     */
    vmm_mapping_info_t code_mapping = {0};
    vmm_mapping_info_t data_mapping = {0};

    bool code_query_ok =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_CODE_VA,
            &code_mapping
        ) == VMM_STATUS_OK;

    bool data_query_ok =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            &data_mapping
        ) == VMM_STATUS_OK;

    bool code_permissions_ok =
        code_query_ok &&
        code_mapping.page_size == 4096ULL &&
        !code_mapping.huge_page &&
        code_mapping.physical_address ==
            physical_by_virtual[0] &&
        code_mapping.user_accessible &&
        !code_mapping.writable &&
        code_mapping.executable;

    bool data_permissions_ok =
        data_query_ok &&
        data_mapping.page_size == 4096ULL &&
        !data_mapping.huge_page &&
        data_mapping.physical_address ==
            physical_by_virtual[1] &&
        data_mapping.user_accessible &&
        data_mapping.writable &&
        !data_mapping.executable;

    if (!code_permissions_ok ||
        !data_permissions_ok) {

        goto cleanup;
    }

    passed = true;

cleanup:

    /*
     * Leave image-loading mode before requesting page cleanup.
     */
    if (loading) {

        if (process_image_load_finish(process, false)) {
            loading = false;
        } else {
            cleanup_ok = false;
        }
    }

    if (attached) {

        if (!loading &&
            process_release_owned_pages(
                process,
                page_set
            )) {

            page_set = NULL;
            attached = false;

        } else {

            cleanup_ok = false;
        }

    } else if (page_set != NULL) {

        if (process_test_release_unattached_page_set(
                page_set
            )) {

            page_set = NULL;

        } else {

            cleanup_ok = false;
        }
    }

    /*
     * Check that successful cleanup removed the mappings
     * and returned both data frames to PMM.
     */
    if (page_set == NULL &&
        physical_by_virtual[0] != 0 &&
        physical_by_virtual[1] != 0) {

        if (!process_test_image_pages_released(
                address_space,
                physical_by_virtual
            )) {

            cleanup_ok = false;
        }
    }

    bool destroyed = process_destroy(process);

    process_put(process);

    return passed && cleanup_ok && destroyed;
}

/*
 * Force the second image-page mapping to fail because another
 * mapping already occupies its virtual address.
 *
 * Verify that:
 *   - The first image page maps successfully.
 *   - The second map operation does not overwrite the existing PTE.
 *   - The unrelated mapping can be removed independently.
 *   - Process cleanup releases both prepared image frames.
 */
static bool process_test_image_mapping_rollback(void)
{
    process_t *process =
        process_create_user("image-rollback-test");

    if (process == NULL) {
        return false;
    }

    vmm_address_space_t *address_space = NULL;

    process_owned_page_set_t *page_set = NULL;

    uint64_t physical_by_virtual[
        PROCESS_IMAGE_TEST_PAGE_COUNT
    ] = {0};

    uint64_t collision_frame = 0;

    bool loading = false;
    bool attached = false;
    bool collision_frame_owned = false;
    bool collision_mapping = false;

    bool passed = false;
    bool cleanup_ok = true;

    if (!process_image_load_begin(
            process,
            &address_space
        )) {

        goto cleanup;
    }

    loading = true;

    if (address_space == NULL) {
        goto cleanup;
    }

    page_set =
        process_test_create_image_page_set(
            physical_by_virtual
        );

    if (page_set == NULL) {
        goto cleanup;
    }

    /*
     * Allocate an unrelated frame and deliberately occupy
     * the data page's virtual address with it.
     *
     * VMM mapping operations do not transfer ownership of
     * this physical frame.
     */
    collision_frame = pmm_alloc_frame();

    if (collision_frame == 0) {
        goto cleanup;
    }

    collision_frame_owned = true;

    vmm_status_t collision_status =
        vmm_address_space_map_page(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            collision_frame,
            VMM_MAP_USER
        );

    if (collision_status != VMM_STATUS_OK) {
        goto cleanup;
    }

    collision_mapping = true;

    /*
     * Transfer the two prepared frames to the process.
     */
    if (!process_image_load_attach_pages(
            process,
            page_set
        )) {

        goto cleanup;
    }

    attached = true;

    uint64_t code_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_CODE_VA
        );

    uint64_t data_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_DATA_VA
        );

    if (code_index == UINT64_MAX ||
        data_index == UINT64_MAX) {

        goto cleanup;
    }

    /*
     * The first page should map normally.
     */
    if (!process_image_load_map_page(
            process,
            page_set,
            code_index
        )) {

        goto cleanup;
    }

    /*
     * The second page must be rejected because its virtual
     * address is already occupied by collision_frame.
     *
     * The existing mapping must remain untouched.
     */
    bool collision_rejected =
        !process_image_load_map_page(
            process,
            page_set,
            data_index
        );

    vmm_mapping_info_t first_mapping = {0};
    vmm_mapping_info_t second_mapping = {0};

    bool first_query_ok =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_CODE_VA,
            &first_mapping
        ) == VMM_STATUS_OK;

    bool second_query_ok =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            &second_mapping
        ) == VMM_STATUS_OK;

    bool first_mapping_intact =
        first_query_ok &&
        first_mapping.page_size == 4096ULL &&
        first_mapping.physical_address ==
            physical_by_virtual[0] &&
        first_mapping.user_accessible &&
        first_mapping.executable &&
        !first_mapping.writable;

    bool collision_mapping_intact =
        second_query_ok &&
        second_mapping.page_size == 4096ULL &&
        second_mapping.physical_address ==
            collision_frame &&
        second_mapping.user_accessible &&
        !second_mapping.writable &&
        !second_mapping.executable;

    if (!collision_rejected ||
        !first_mapping_intact ||
        !collision_mapping_intact) {

        goto cleanup;
    }

    /*
     * Mark the image load as failed. The attached page set
     * remains process-owned and can now be cleaned up.
     */
    if (!process_image_load_finish(process, false)) {
        goto cleanup;
    }

    loading = false;

    /*
     * Remove the unrelated mapping before releasing the
     * process-owned image pages.
     */
    uint64_t unmapped_collision_frame = 0;

    vmm_status_t unmap_status =
        vmm_address_space_unmap_page(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            &unmapped_collision_frame
        );

    if (unmap_status != VMM_STATUS_OK ||
        unmapped_collision_frame != collision_frame) {

        goto cleanup;
    }

    collision_mapping = false;

    if (!pmm_free_frame(collision_frame)) {
        goto cleanup;
    }

    collision_frame_owned = false;

    /*
     * The process owns both prepared frames.
     *
     * Cleanup must unmap and release the first frame, and
     * release the second frame, which was never mapped.
     */
    if (!process_release_owned_pages(
            process,
            page_set
        )) {

        goto cleanup;
    }

    page_set = NULL;
    attached = false;

    bool pages_released =
        process_test_image_pages_released(
            address_space,
            physical_by_virtual
        );

    bool collision_frame_released =
        pmm_frame_to_virt(collision_frame) == NULL;

    passed =
        pages_released &&
        collision_frame_released;

cleanup:

    /*
     * End image construction before attempting process-owned
     * page cleanup.
     */
    if (loading) {

        if (process_image_load_finish(process, false)) {
            loading = false;
        } else {
            cleanup_ok = false;
        }
    }

    /*
     * Remove the unrelated mapping first. Otherwise cleanup
     * would correctly identify an unexpected physical frame
     * at the image data address and quarantine the record.
     */
    if (collision_mapping) {

        uint64_t unmapped_physical = 0;

        vmm_status_t status =
            vmm_address_space_unmap_page(
                address_space,
                PROCESS_IMAGE_TEST_DATA_VA,
                &unmapped_physical
            );

        if (status == VMM_STATUS_OK &&
            unmapped_physical == collision_frame) {

            collision_mapping = false;

        } else {

            cleanup_ok = false;
        }
    }

    /*
     * The collision frame is caller-owned independently
     * of the process image page set.
     */
    if (collision_frame_owned &&
        !collision_mapping) {

        if (pmm_free_frame(collision_frame)) {
            collision_frame_owned = false;
        } else {
            cleanup_ok = false;
        }
    }

    if (attached) {

        if (!loading &&
            !collision_mapping &&
            process_release_owned_pages(
                process,
                page_set
            )) {

            page_set = NULL;
            attached = false;

        } else {

            cleanup_ok = false;
        }

    } else if (page_set != NULL) {

        if (process_test_release_unattached_page_set(
                page_set
            )) {

            page_set = NULL;

        } else {

            cleanup_ok = false;
        }
    }

    bool destroyed = process_destroy(process);

    process_put(process);

    return passed && cleanup_ok && destroyed;
}

/*
 * Create a process with both test image pages mapped and attached.
 *
 * On success, the caller owns the process reference and borrows
 * the page-set and address-space pointers for testing.
 */
static bool process_test_create_mapped_image_for_cleanup(
    process_t **process_out,
    vmm_address_space_t **address_space_out,
    process_owned_page_set_t **page_set_out,
    uint64_t physical_by_virtual[PROCESS_IMAGE_TEST_PAGE_COUNT]
) {
    if (process_out == NULL ||
        address_space_out == NULL ||
        page_set_out == NULL ||
        physical_by_virtual == NULL) {

        return false;
    }

    *process_out = NULL;
    *address_space_out = NULL;
    *page_set_out = NULL;

    physical_by_virtual[0] = 0;
    physical_by_virtual[1] = 0;

    process_t *process =
        process_create_user("cleanup-failure-test");

    if (process == NULL) {
        return false;
    }

    vmm_address_space_t *address_space = NULL;
    process_owned_page_set_t *page_set = NULL;

    bool loading = false;
    bool attached = false;

    if (!process_image_load_begin(
            process,
            &address_space
        )) {

        goto cleanup;
    }

    loading = true;

    if (address_space == NULL) {
        goto cleanup;
    }

    page_set =
        process_test_create_image_page_set(
            physical_by_virtual
        );

    if (page_set == NULL) {
        goto cleanup;
    }

    if (!process_image_load_attach_pages(
            process,
            page_set
        )) {

        goto cleanup;
    }

    attached = true;

    const uint64_t virtual_addresses[
        PROCESS_IMAGE_TEST_PAGE_COUNT
    ] = {
        PROCESS_IMAGE_TEST_CODE_VA,
        PROCESS_IMAGE_TEST_DATA_VA
    };

    for (uint64_t i = 0;
         i < PROCESS_IMAGE_TEST_PAGE_COUNT;
         i++) {

        uint64_t page_index =
            process_test_find_image_page(
                page_set,
                virtual_addresses[i]
            );

        if (page_index == UINT64_MAX ||
            !process_image_load_map_page(
                process,
                page_set,
                page_index
            )) {

            goto cleanup;
        }
    }

    /*
     * The test is exercising cleanup of an image whose
     * construction has ended unsuccessfully.
     */
    if (!process_image_load_finish(process, false)) {
        goto cleanup;
    }

    loading = false;

    *process_out = process;
    *address_space_out = address_space;
    *page_set_out = page_set;

    return true;

cleanup:

    if (loading) {
        if (process_image_load_finish(process, false)) {
            loading = false;
        }
    }

    if (attached && !loading) {
        if (process_release_owned_pages(
                process,
                page_set
            )) {

            page_set = NULL;
        }

    } else if (!attached && page_set != NULL) {

        if (process_test_release_unattached_page_set(
                page_set
            )) {

            page_set = NULL;
        }
    }

    /*
     * Final process cleanup retries any page set that remained
     * attached after the direct cleanup attempt.
     */
    (void)process_destroy(process);
    process_put(process);

    return false;
}

/*
 * Force one process-owned physical-frame release to fail.
 *
 * Required behavior:
 *   - Both virtual mappings are removed.
 *   - The selected frame remains allocated and process-owned.
 *   - The unrelated frame is released during the same attempt.
 *   - Page-set metadata remains attached for retry.
 *   - A second cleanup attempt releases the remaining frame.
 */
static bool process_test_cleanup_frame_free_failure_retry(void)
{
    process_t *process = NULL;
    vmm_address_space_t *address_space = NULL;
    process_owned_page_set_t *page_set = NULL;

    uint64_t physical_by_virtual[
        PROCESS_IMAGE_TEST_PAGE_COUNT
    ] = {0};

    bool passed = false;
    bool cleanup_ok = true;

    if (!process_test_create_mapped_image_for_cleanup(
            &process,
            &address_space,
            &page_set,
            physical_by_virtual
        )) {

        return false;
    }

    uint64_t target_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_DATA_VA
        );

    uint64_t other_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_CODE_VA
        );

    if (target_index == UINT64_MAX ||
        other_index == UINT64_MAX) {

        goto cleanup;
    }

    uint64_t target_physical =
        page_set->pages[target_index].physical_address;

    uint64_t other_physical =
        page_set->pages[other_index].physical_address;

    if (!pmm_test_fail_free_frame_once(target_physical)) {
        goto cleanup;
    }

    /*
     * This must fail because one frame could not be released.
     * The other record must nevertheless make progress.
     */
    if (process_release_owned_pages(
            process,
            page_set
        )) {

        page_set = NULL;
        goto cleanup;
    }

    /*
     * Inspect the retained records, not just the return value.
     */
    process_owned_page_t *target =
        &page_set->pages[target_index];

    process_owned_page_t *other =
        &page_set->pages[other_index];

    bool target_record_safe =
        target->frame_owned &&
        !target->mapped &&
        !target->quarantined;

    bool other_record_released =
        !other->frame_owned &&
        !other->mapped &&
        !other->quarantined;

    vmm_mapping_info_t mapping = {0};

    bool both_unmapped =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            &mapping
        ) == VMM_STATUS_NOT_MAPPED &&
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_CODE_VA,
            &mapping
        ) == VMM_STATUS_NOT_MAPPED;

    bool physical_ownership_correct =
        pmm_frame_to_virt(target_physical) != NULL &&
        pmm_frame_to_virt(other_physical) == NULL;

    if (!target_record_safe ||
        !other_record_released ||
        !both_unmapped ||
        !physical_ownership_correct) {

        goto cleanup;
    }

    /*
     * The one-shot PMM failure has been consumed. Retry.
     */
    if (!process_release_owned_pages(
            process,
            page_set
        )) {

        goto cleanup;
    }

    page_set = NULL;

    passed =
        process_test_image_pages_released(
            address_space,
            physical_by_virtual
        );

cleanup:

    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();
    vmm_test_clear_address_space_unmap_failure();

    /*
     * If an assertion failed while the page set remained attached,
     * retry cleanup after removing all injected failures.
     */
    if (page_set != NULL) {

        if (process_release_owned_pages(
                process,
                page_set
            )) {

            page_set = NULL;

        } else {

            cleanup_ok = false;
        }
    }

    bool destroyed = process_destroy(process);

    process_put(process);

    return passed && cleanup_ok && destroyed;
}

/*
 * Force one process-owned page unmap to fail.
 *
 * Required behavior:
 *   - The selected mapping remains unchanged.
 *   - Its physical frame remains allocated and owned.
 *   - The other page is unmapped and released.
 *   - Cleanup retains the page set.
 *   - A retry completes the remaining unmap and frame release.
 */
static bool process_test_cleanup_unmap_failure_retry(void)
{
    process_t *process = NULL;
    vmm_address_space_t *address_space = NULL;
    process_owned_page_set_t *page_set = NULL;

    uint64_t physical_by_virtual[
        PROCESS_IMAGE_TEST_PAGE_COUNT
    ] = {0};

    bool passed = false;
    bool cleanup_ok = true;

    if (!process_test_create_mapped_image_for_cleanup(
            &process,
            &address_space,
            &page_set,
            physical_by_virtual
        )) {

        return false;
    }

    uint64_t target_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_CODE_VA
        );

    uint64_t other_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_DATA_VA
        );

    if (target_index == UINT64_MAX ||
        other_index == UINT64_MAX) {

        goto cleanup;
    }

    uint64_t target_physical =
        page_set->pages[target_index].physical_address;

    uint64_t other_physical =
        page_set->pages[other_index].physical_address;

    /*
     * The hook fails once before modifying the selected PTE.
     */
    vmm_test_fail_address_space_unmap_at(
        PROCESS_IMAGE_TEST_CODE_VA
    );

    /*
     * Cleanup must report incomplete progress.
     */
    if (process_release_owned_pages(
            process,
            page_set
        )) {

        page_set = NULL;
        goto cleanup;
    }

    process_owned_page_t *target =
        &page_set->pages[target_index];

    process_owned_page_t *other =
        &page_set->pages[other_index];

    /*
     * The selected record must still describe a live mapping
     * and an allocated frame.
     */
    vmm_mapping_info_t target_mapping = {0};

    bool target_mapping_intact =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_CODE_VA,
            &target_mapping
        ) == VMM_STATUS_OK &&
        target_mapping.page_size == 4096ULL &&
        target_mapping.physical_address == target_physical &&
        target_mapping.user_accessible &&
        target_mapping.executable &&
        !target_mapping.writable;

    bool target_record_intact =
        target->frame_owned &&
        target->mapped &&
        !target->quarantined &&
        pmm_frame_to_virt(target_physical) != NULL;

    /*
     * The other page should have been completely released,
     * despite the selected page's unmap failure.
     */
    vmm_mapping_info_t other_mapping = {0};

    bool other_mapping_removed =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            &other_mapping
        ) == VMM_STATUS_NOT_MAPPED;

    bool other_record_released =
        !other->frame_owned &&
        !other->mapped &&
        !other->quarantined &&
        pmm_frame_to_virt(other_physical) == NULL;

    if (!target_mapping_intact ||
        !target_record_intact ||
        !other_mapping_removed ||
        !other_record_released) {

        goto cleanup;
    }

    /*
     * The unmap failure is one-shot. The next cleanup should
     * remove the remaining mapping and release its frame.
     */
    if (!process_release_owned_pages(
            process,
            page_set
        )) {

        goto cleanup;
    }

    page_set = NULL;

    passed =
        process_test_image_pages_released(
            address_space,
            physical_by_virtual
        );

cleanup:

    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();
    vmm_test_clear_address_space_unmap_failure();

    if (page_set != NULL) {

        if (process_release_owned_pages(
                process,
                page_set
            )) {

            page_set = NULL;

        } else {

            cleanup_ok = false;
        }
    }

    bool destroyed = process_destroy(process);

    process_put(process);

    return passed && cleanup_ok && destroyed;
}


/*
 * Force final process cleanup to encounter a physical-frame
 * release failure.
 *
 * Required behavior:
 *   - process_destroy() removes the process from the registry.
 *   - The first process_put() cannot release the selected frame.
 *   - The final process reference and address space are retained.
 *   - Other process-owned frames are still released.
 *   - Both image mappings are removed.
 *   - A second process_put() completes the remaining cleanup.
 */
static bool process_test_final_put_frame_free_failure_retry(void)
{
    process_t *process = NULL;
    vmm_address_space_t *address_space = NULL;
    process_owned_page_set_t *page_set = NULL;

    uint64_t physical_by_virtual[
        PROCESS_IMAGE_TEST_PAGE_COUNT
    ] = {0};

    bool destroyed = false;
    bool passed = false;
    bool cleanup_ok = true;

    if (!process_test_create_mapped_image_for_cleanup(
            &process,
            &address_space,
            &page_set,
            physical_by_virtual
        )) {

        return false;
    }

    uint64_t target_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_DATA_VA
        );

    uint64_t other_index =
        process_test_find_image_page(
            page_set,
            PROCESS_IMAGE_TEST_CODE_VA
        );

    if (target_index == UINT64_MAX ||
        other_index == UINT64_MAX) {

        goto cleanup;
    }

    uint64_t target_physical =
        page_set->pages[target_index].physical_address;

    uint64_t other_physical =
        page_set->pages[other_index].physical_address;

    /*
     * Remove the process from the registry.
     *
     * The caller's reference remains, so process_put()
     * will become responsible for final resource cleanup.
     */
    if (!process_destroy(process)) {
        goto cleanup;
    }

    destroyed = true;

    /*
     * Fail exactly one release of the selected data frame.
     */
    if (!pmm_test_fail_free_frame_once(target_physical)) {
        goto cleanup;
    }

    /*
     * process_put() returns void. The injected failure must
     * therefore be verified through the resources it manages.
     */
    process_put(process);

    /*
     * Check the physical frame first.
     *
     * If it was unexpectedly released, do not dereference
     * the process pointer: final process destruction may
     * already have freed the descriptor.
     */
    if (pmm_frame_to_virt(target_physical) == NULL) {
        process = NULL;
        goto cleanup;
    }

    /*
     * The other frame should have been released even though
     * cleanup of the selected frame failed.
     */
    bool other_frame_released =
        pmm_frame_to_virt(other_physical) == NULL;

    /*
     * Verify that the process descriptor and its address
     * space survived the failed final cleanup.
     */
    vmm_address_space_t *retained_address_space = NULL;

    bool process_reference_retained =
        process_get_address_space(
            process,
            &retained_address_space
        ) &&
        retained_address_space == address_space;

    /*
     * Both mappings should have been removed before the
     * physical-frame release failure was encountered.
     */
    vmm_mapping_info_t mapping = {0};

    bool target_unmapped =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_DATA_VA,
            &mapping
        ) == VMM_STATUS_NOT_MAPPED;

    bool other_unmapped =
        vmm_address_space_query(
            address_space,
            PROCESS_IMAGE_TEST_CODE_VA,
            &mapping
        ) == VMM_STATUS_NOT_MAPPED;

    if (!other_frame_released ||
        !process_reference_retained ||
        !target_unmapped ||
        !other_unmapped) {

        goto cleanup;
    }

    /*
     * The failed PMM operation was one-shot. Clear the
     * injection explicitly before retrying final cleanup.
     */
    pmm_test_clear_free_frame_failure();

    /*
     * Retry final process destruction.
     *
     * On successful completion, the process descriptor is
     * freed. Do not access process after this call.
     */
    process_put(process);
    process = NULL;

    passed =
        pmm_frame_to_virt(target_physical) == NULL &&
        pmm_frame_to_virt(other_physical) == NULL;

cleanup:

    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();
    vmm_test_clear_address_space_unmap_failure();

    /*
     * If the test stopped before final destruction, make
     * a best-effort cleanup attempt with all injections off.
     */
    if (process != NULL) {

        if (!destroyed) {

            destroyed = process_destroy(process);

            if (!destroyed) {
                cleanup_ok = false;
            }
        }

        if (destroyed) {
            process_put(process);
            process = NULL;
        }
    }

    return passed && cleanup_ok && destroyed;
}


/*
 * Force final process cleanup to encounter a failure while
 * releasing its private VMM address-space root.
 *
 * This test uses an empty process address space:
 *   - No process-owned image pages exist.
 *   - No private lower-level page tables have been allocated.
 *   - The PML4 root is therefore the first and only private
 *     page-table frame released by vmm_address_space_put().
 *
 * Required behavior:
 *   - The first process_put() reaches VMM address-space teardown.
 *   - PMM rejects release of the exact PML4 root frame.
 *   - VMM retains the registered address-space object and root.
 *   - process_put() retains the final process reference.
 *   - The process can still retrieve its original address space.
 *   - A second process_put() successfully releases the root
 *     and destroys the process descriptor.
 */
static bool process_test_final_put_address_space_free_failure_retry(void)
{
    process_t *process = NULL;

    vmm_address_space_t *address_space = NULL;

    uint64_t root_physical = 0;
    uint64_t retained_root_physical = 0;

    bool destroyed = false;
    bool passed = false;
    bool cleanup_ok = true;

    /*
     * Create a user process with an independently owned,
     * initially empty address space.
     */
    process = process_create_user(
        "final-put-vmm-failure-test"
    );

    if (process == NULL) {
        goto cleanup;
    }

    /*
     * Retrieve the borrowed address-space pointer.
     *
     * The process retains ownership of its initial VMM
     * address-space reference.
     */
    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL ||
        address_space == vmm_kernel_address_space()) {

        goto cleanup;
    }

    /*
     * Obtain the physical PML4 root.
     */
    if (!vmm_address_space_get_root_physical(
            address_space,
            &root_physical
        ) ||
        root_physical == 0 ||
        (root_physical & 0xFFFULL) != 0 ||
        pmm_frame_to_virt(root_physical) == NULL) {

        goto cleanup;
    }

    /*
     * Confirm that the private address space is empty.
     *
     * This is important: phase one of process_put() must
     * not release image frames, and VMM must not have any
     * private lower-level page tables to reclaim.
     */
    vmm_mapping_info_t mapping = {0};

    if (vmm_address_space_query(
            address_space,
            0x00400000ULL,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto cleanup;
    }

    /*
     * Remove the process from the registry.
     *
     * The caller still owns one process reference.
     * Its next process_put() will attempt final cleanup.
     */
    if (!process_destroy(process)) {
        goto cleanup;
    }

    destroyed = true;

    /*
     * Inject one failure for the exact PML4 root frame.
     *
     * No other PMM frame release should occur before
     * vmm_address_space_put() attempts to free this root.
     */
    if (!pmm_test_fail_free_frame_once(root_physical)) {
        goto cleanup;
    }

    /*
     * Attempt final cleanup.
     *
     * process_put() returns void, so verify the failure
     * through the resources that must remain alive.
     */
    process_put(process);

    /*
     * Check the root frame before dereferencing either
     * the process descriptor or address-space pointer.
     *
     * If the frame was unexpectedly freed, final cleanup
     * may already have destroyed both objects.
     */
    if (pmm_frame_to_virt(root_physical) == NULL) {
        process = NULL;
        goto cleanup;
    }

    /*
     * Verify that the process descriptor retained its
     * final reference and still exposes the same address
     * space after the failed cleanup attempt.
     */
    vmm_address_space_t *retained_address_space = NULL;

    bool process_reference_retained =
        process_get_address_space(
            process,
            &retained_address_space
        ) &&
        retained_address_space == address_space;

    /*
     * Verify that VMM retained the registered address-space
     * object and its original physical PML4 root.
     */
    bool address_space_retained =
        vmm_address_space_get_root_physical(
            address_space,
            &retained_root_physical
        ) &&
        retained_root_physical == root_physical;

    /*
     * The address space must still be queryable.
     * Its previously unmapped address must remain unmapped.
     */
    bool address_space_queryable =
        vmm_address_space_query(
            address_space,
            0x00400000ULL,
            &mapping
        ) == VMM_STATUS_NOT_MAPPED;

    /*
     * The injected PMM failure must not have corrupted
     * the root frame's allocation state.
     */
    bool root_still_allocated =
        pmm_frame_to_virt(root_physical) != NULL;

    if (!process_reference_retained ||
        !address_space_retained ||
        !address_space_queryable ||
        !root_still_allocated) {

        goto cleanup;
    }

    /*
     * Clear failure injection before retrying.
     */
    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * Retry final cleanup.
     *
     * On success, both the address-space object and
     * process descriptor are destroyed.
     *
     * Do not access either pointer after this call.
     */
    process_put(process);
    process = NULL;

    /*
     * The PML4 root frame must now be free.
     */
    passed =
        pmm_frame_to_virt(root_physical) == NULL;

cleanup:

    /*
     * Always clear failure injection, including paths
     * where setup or an earlier assertion failed.
     */
    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * Best-effort cleanup if the process still has a
     * valid caller reference.
     */
    if (process != NULL) {

        if (!destroyed) {

            destroyed = process_destroy(process);

            if (!destroyed) {
                cleanup_ok = false;
            }
        }

        if (destroyed) {

            process_put(process);
            process = NULL;
        }
    }

    return passed && cleanup_ok && destroyed;
}


/*
 * Exercise process final-cleanup retry after partial
 * reclamation of the private page-table hierarchy.
 *
 * One 4 KiB user mapping in an initially empty address space
 * creates three lower-level page tables:
 *
 *     PML4 -> PDPT -> PD -> PT
 *
 * The PML4 root is released last.
 *
 * We test two failure positions:
 *     1. One successful table release, then failure.
 *     2. Two successful table releases, then failure.
 *
 * The mapped data frame belongs to the test, not the process.
 * VMM must never release that data frame during address-space
 * teardown.
 */
static bool process_test_final_put_partial_teardown_case(
    uint64_t successful_frees_before_failure
) {
    process_t *process = NULL;

    vmm_address_space_t *address_space = NULL;

    uint64_t root_physical = 0;
    uint64_t data_physical = 0;

    bool destroyed = false;
    bool passed = false;
    bool cleanup_ok = true;

    const uint64_t test_virtual_address =
        0x00400000ULL;

    /*
     * Only the two intended intermediate failure positions
     * are valid for this test.
     */
    if (successful_frees_before_failure != 1 &&
        successful_frees_before_failure != 2) {

        return false;
    }

    /*
     * Create a process with an empty private address space.
     */
    process = process_create_user(
        "partial-vmm-teardown-test"
    );

    if (process == NULL) {
        goto cleanup;
    }

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL ||
        address_space == vmm_kernel_address_space()) {

        goto cleanup;
    }

    /*
     * Record the PML4 root physical address.
     */
    if (!vmm_address_space_get_root_physical(
            address_space,
            &root_physical
        ) ||
        root_physical == 0 ||
        (root_physical & 0xFFFULL) != 0 ||
        pmm_frame_to_virt(root_physical) == NULL) {

        goto cleanup;
    }

    /*
     * Allocate a caller-owned data frame.
     *
     * process_put() must not release this frame because
     * it is not attached to a process-owned page set.
     */
    data_physical = pmm_alloc_frame();

    if (data_physical == 0 ||
        pmm_frame_to_virt(data_physical) == NULL) {

        goto cleanup;
    }

    /*
     * Create exactly one 4 KiB user mapping.
     *
     * This creates the private lower-level page-table path
     * needed to exercise recursive teardown.
     */
    if (vmm_address_space_map_page(
            address_space,
            test_virtual_address,
            data_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        goto cleanup;
    }

    /*
     * Verify that the mapping exists before teardown.
     */
    vmm_mapping_info_t mapping = {0};

    if (vmm_address_space_query(
            address_space,
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_OK) {

        goto cleanup;
    }

    /*
     * Remove the process from the registry.
     *
     * The caller's reference remains available for final
     * cleanup.
     */
    if (!process_destroy(process)) {
        goto cleanup;
    }

    destroyed = true;

    /*
     * Permit N successful page-table frame releases.
     * The next valid release must fail.
     *
     * Phase one of process_put() has no process-owned
     * image frames to release, so this injection reaches
     * the VMM page-table reclamation phase.
     */
    if (!pmm_test_fail_free_after(
            successful_frees_before_failure
        )) {

        goto cleanup;
    }

    /*
     * First final-cleanup attempt.
     */
    process_put(process);

    /*
     * The PML4 root must still exist after the injected
     * intermediate page-table release failure.
     *
     * Check physical state before dereferencing process
     * or address_space pointers.
     */
    if (pmm_frame_to_virt(root_physical) == NULL) {
        process = NULL;
        goto cleanup;
    }

    /*
     * Verify that process_put() retained the process's
     * final reference and address-space ownership.
     */
    vmm_address_space_t *retained_address_space = NULL;

    bool process_retained =
        process_get_address_space(
            process,
            &retained_address_space
        ) &&
        retained_address_space == address_space;

    /*
     * Verify that the VMM object remains registered and
     * still identifies the original PML4 root.
     */
    uint64_t retained_root_physical = 0;

    bool root_retained =
        vmm_address_space_get_root_physical(
            address_space,
            &retained_root_physical
        ) &&
        retained_root_physical == root_physical;

    /*
     * At least the lowest-level PT must have been reclaimed.
     * Consequently, the original mapping must no longer
     * be accessible through this address space.
     */
    bool mapping_removed =
        vmm_address_space_query(
            address_space,
            test_virtual_address,
            &mapping
        ) == VMM_STATUS_NOT_MAPPED;

    /*
     * The physical data frame is caller-owned.
     * Reclaiming page tables must not free it.
     */
    bool data_frame_preserved =
        pmm_frame_to_virt(data_physical) != NULL;

    if (!process_retained ||
        !root_retained ||
        !mapping_removed ||
        !data_frame_preserved) {

        goto cleanup;
    }

    /*
     * Disable failure injection before retry.
     */
    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * Retry final process cleanup.
     */
    process_put(process);

    /*
     * If the PML4 root is gone, VMM completed its final
     * teardown. As in the other final-put tests, do not
     * dereference the process or address-space pointers.
     */
    if (pmm_frame_to_virt(root_physical) == NULL) {

        process = NULL;

        passed =
            pmm_frame_to_virt(data_physical) != NULL;
    }

cleanup:

    /*
     * Clear both PMM failure-injection modes regardless
     * of which assertion failed.
     */
    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * Best-effort process cleanup if the test still has
     * a valid caller reference.
     */
    if (process != NULL) {

        if (!destroyed) {

            destroyed = process_destroy(process);

            if (!destroyed) {
                cleanup_ok = false;
            }
        }

        if (destroyed) {

            /*
             * Retry with failure injection disabled.
             */
            process_put(process);

            if (root_physical != 0 &&
                pmm_frame_to_virt(root_physical) == NULL) {

                process = NULL;

            } else {

                /*
                 * If cleanup did not finish, the retained
                 * final reference should still permit retry.
                 */
                process_put(process);

                if (root_physical != 0 &&
                    pmm_frame_to_virt(root_physical) == NULL) {

                    process = NULL;

                } else {

                    cleanup_ok = false;
                }
            }
        }
    }

    /*
     * The data frame can only be freed once the address
     * space root is gone. This avoids freeing a frame that
     * might still be referenced by a live page table.
     */
    if (data_physical != 0 &&
        pmm_frame_to_virt(data_physical) != NULL) {

        if (root_physical != 0 &&
            pmm_frame_to_virt(root_physical) == NULL) {

            if (!pmm_free_frame(data_physical) &&
                pmm_frame_to_virt(data_physical) != NULL) {

                cleanup_ok = false;
            }

        } else {

            /*
             * Preserve the frame rather than risk freeing
             * memory that might still be mapped.
             */
            cleanup_ok = false;
        }
    }

    return passed && cleanup_ok && destroyed;
}


/*
 * Run both partial page-table reclamation failure positions.
 */
static bool process_test_final_put_partial_teardown_retry(void)
{
    bool fail_after_one =
        process_test_final_put_partial_teardown_case(1);

    console_write(fail_after_one
        ? "  [ OK ] Partial page-table teardown after one release\n"
        : "  [FAIL] Partial page-table teardown after one release\n");

    bool fail_after_two =
        process_test_final_put_partial_teardown_case(2);

    console_write(fail_after_two
        ? "  [ OK ] Partial page-table teardown after two releases\n"
        : "  [FAIL] Partial page-table teardown after two releases\n");

    return fail_after_one && fail_after_two;
}

bool process_run_tests(void)
{
    console_write("[TEST] Process manager\n");

    bool create_lookup = process_test_create_and_lookup();
    console_write(create_lookup
        ? "  [ OK ] Create and lookup\n"
        : "  [FAIL] Create and lookup\n");

    bool thread_lifecycle = process_test_thread_lifecycle();
    console_write(thread_lifecycle
        ? "  [ OK ] Thread lifecycle\n"
        : "  [FAIL] Thread lifecycle\n");

    bool multiple_threads = process_test_multiple_threads();
    console_write(multiple_threads
        ? "  [ OK ] Multiple threads\n"
        : "  [FAIL] Multiple threads\n");

    bool live_thread_destroy = process_test_destroy_with_live_thread();
    console_write(live_thread_destroy
        ? "  [ OK ] Reject destroy with live thread\n"
        : "  [FAIL] Reject destroy with live thread\n");

    bool thread_integration =
        process_test_thread_integration();
    console_write(thread_integration
        ? "  [ OK ] Process-thread integration\n"
        : "  [FAIL] Process-thread integration\n");

    bool address_space_ownership =
        process_test_address_space_ownership();

    console_write(address_space_ownership
        ? "  [ OK ] Process address-space ownership\n"
        : "  [FAIL] Process address-space ownership\n");

    bool image_page_mapping =
        process_test_image_page_mapping();

    console_write(image_page_mapping
        ? "  [ OK ] Process image-page mapping and permissions\n"
        : "  [FAIL] Process image-page mapping and permissions\n");

    bool image_mapping_rollback =
        process_test_image_mapping_rollback();

    console_write(image_mapping_rollback
        ? "  [ OK ] Process image-mapping rollback\n"
        : "  [FAIL] Process image-mapping rollback\n");

    bool cleanup_free_failure =
        process_test_cleanup_frame_free_failure_retry();

    console_write(cleanup_free_failure
        ? "  [ OK ] Owned-page frame-free failure and retry\n"
        : "  [FAIL] Owned-page frame-free failure and retry\n");

    bool cleanup_unmap_failure =
        process_test_cleanup_unmap_failure_retry();

    console_write(cleanup_unmap_failure
        ? "  [ OK ] Owned-page unmap failure and retry\n"
        : "  [FAIL] Owned-page unmap failure and retry\n");

    bool final_put_retry =
        process_test_final_put_frame_free_failure_retry();

    console_write(final_put_retry
        ? "  [ OK ] Final process cleanup failure and retry\n"
        : "  [FAIL] Final process cleanup failure and retry\n");


    bool final_put_address_space_retry =
        process_test_final_put_address_space_free_failure_retry();

    console_write(final_put_address_space_retry
        ? "  [ OK ] Final address-space teardown failure and retry\n"
        : "  [FAIL] Final address-space teardown failure and retry\n");

    bool final_put_partial_teardown_retry =
        process_test_final_put_partial_teardown_retry();

    console_write(final_put_partial_teardown_retry
        ? "  [ OK ] Partial address-space teardown and retry\n"
        : "  [FAIL] Partial address-space teardown and retry\n");

    bool exit_authorization =
        process_test_exit_authorization();

    console_write(exit_authorization
        ? "  [ OK ] Atomic process-exit authorization\n"
        : "  [FAIL] Atomic process-exit authorization\n");

    bool passed =
        create_lookup &&
        thread_lifecycle &&
        multiple_threads &&
        live_thread_destroy &&
        thread_integration &&
        address_space_ownership &&
        image_page_mapping &&
        image_mapping_rollback &&
        cleanup_free_failure &&
        cleanup_unmap_failure &&
        final_put_retry &&
        final_put_address_space_retry &&
        final_put_partial_teardown_retry &&
        exit_authorization;

    console_write(passed
        ? "[ OK ] Process manager tests passed\n"
        : "[FAIL] Process manager tests failed\n");

    return passed;
}
