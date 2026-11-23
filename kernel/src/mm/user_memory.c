
#include "mm/user_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/pmm.h"
#include "mm/vmm.h"

#include "scheduler/thread.h"


/*
 * HUA currently uses four-level x86-64 paging.
 *
 * The lower canonical half is available for user mappings.
 * The upper half is reserved for supervisor mappings.
 */
#define USER_MEMORY_USER_LIMIT \
    0x0000800000000000ULL


/*
 * An operation holds a temporary address-space reference
 * and one level of preemption protection.
 */
struct user_memory_context {

    vmm_address_space_t *space;

    bool reference_held;

    bool preemption_disabled;
};


/*
 * Validate the access bitmask.
 */
static bool user_memory_access_is_valid(
    user_memory_access_t access
) {

    const uint32_t allowed =
        USER_MEMORY_ACCESS_READ |
        USER_MEMORY_ACCESS_WRITE;

    uint32_t requested = (uint32_t)access;

    return
        requested != 0 &&
        (requested & ~allowed) == 0;
}


/*
 * Enter a protected user-memory operation.
 *
 * First acquire a reference so the address-space object
 * remains alive during the operation.
 *
 * Then disable involuntary preemption so CPU0 cannot switch
 * to another process while the mappings are being inspected
 * and copied.
 */
static user_memory_status_t user_memory_context_begin(
    vmm_address_space_t *space,
    struct user_memory_context *context
) {

    if (space == NULL || context == NULL) {
        return USER_MEMORY_STATUS_INVALID_ARGUMENT;
    }

    context->space = NULL;

    context->reference_held = false;

    context->preemption_disabled = false;

    if (!vmm_address_space_get(space)) {
        return USER_MEMORY_STATUS_ADDRESS_SPACE_ERROR;
    }

    context->space = space;

    context->reference_held = true;

    if (!thread_preempt_disable()) {

        bool released =
            vmm_address_space_put(space);

        context->space = NULL;

        context->reference_held = false;

        return released
            ? USER_MEMORY_STATUS_PREEMPTION_ERROR
            : USER_MEMORY_STATUS_INTERNAL_ERROR;
    }

    context->preemption_disabled = true;

    return USER_MEMORY_STATUS_OK;
}


/*
 * Leave a protected operation.
 *
 * Preemption protection is released before dropping the
 * temporary address-space reference.
 */
static user_memory_status_t user_memory_context_finish(
    struct user_memory_context *context,
    user_memory_status_t operation_status
) {

    if (context == NULL) {
        return USER_MEMORY_STATUS_INTERNAL_ERROR;
    }

    bool preemption_ok = true;

    bool reference_ok = true;

    if (context->preemption_disabled) {

        preemption_ok = thread_preempt_enable();

        context->preemption_disabled = false;
    }

    if (context->reference_held) {

        reference_ok =
            vmm_address_space_put(context->space);

        context->reference_held = false;

        context->space = NULL;
    }

    if (!preemption_ok || !reference_ok) {
        return USER_MEMORY_STATUS_INTERNAL_ERROR;
    }

    return operation_status;
}


/*
 * Validate arithmetic and lower-half user bounds.
 *
 * This function does not inspect page tables.
 */
static user_memory_status_t user_memory_check_bounds(
    uint64_t user_address,
    size_t length
) {

    if (length == 0) {
        return USER_MEMORY_STATUS_OK;
    }

    if ((uint64_t)length > USER_MEMORY_MAX_COPY_BYTES) {
        return USER_MEMORY_STATUS_TOO_LARGE;
    }

    uint64_t length_minus_one =
        (uint64_t)length - 1ULL;

    /*
     * Reject integer overflow before calculating the
     * inclusive last byte.
     */
    if (user_address > UINT64_MAX - length_minus_one) {
        return USER_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    uint64_t last_address =
        user_address + length_minus_one;

    /*
     * A syscall argument must never refer to:
     * - The noncanonical hole.
     * - The upper-half kernel address space.
     */
    if (user_address >= USER_MEMORY_USER_LIMIT ||
        last_address >= USER_MEMORY_USER_LIMIT) {

        return USER_MEMORY_STATUS_NOT_USER_RANGE;
    }

    return USER_MEMORY_STATUS_OK;
}


/*
 * Validate every page in a user range.
 *
 * When page_aliases is non-NULL, each validated physical
 * page is also converted into its HHDM virtual alias.
 *
 * The caller must already hold:
 * - An address-space reference.
 * - Preemption protection.
 *
 * The page-alias array has USER_MEMORY_MAX_PAGES entries.
 */
static user_memory_status_t user_memory_preflight(
    vmm_address_space_t *space,
    uint64_t user_address,
    size_t length,
    user_memory_access_t access,
    void **page_aliases,
    size_t *page_count_out
) {

    if (space == NULL ||
        !user_memory_access_is_valid(access)) {

        return USER_MEMORY_STATUS_INVALID_ARGUMENT;
    }

    if (page_count_out != NULL) {
        *page_count_out = 0;
    }

    user_memory_status_t bounds_status =
        user_memory_check_bounds(
            user_address,
            length
        );

    if (bounds_status != USER_MEMORY_STATUS_OK ||
        length == 0) {

        return bounds_status;
    }

    uint64_t last_address =
        user_address + (uint64_t)length - 1ULL;

    uint64_t first_page =
        user_address & ~(PMM_PAGE_SIZE - 1ULL);

    uint64_t last_page =
        last_address & ~(PMM_PAGE_SIZE - 1ULL);

    size_t page_count =
        (size_t)(((last_page - first_page) /
                  PMM_PAGE_SIZE) + 1ULL);

    if (page_count > USER_MEMORY_MAX_PAGES) {
        return USER_MEMORY_STATUS_INTERNAL_ERROR;
    }

    /*
     * Inspect every page before copying any byte.
     *
     * This ensures that an unmapped or non-writable page
     * later in the range cannot cause a partial copy.
     */
    for (size_t index = 0;
         index < page_count;
         index++) {

        uint64_t virtual_page =
            first_page +
            ((uint64_t)index * PMM_PAGE_SIZE);

        vmm_mapping_info_t mapping = {0};

        vmm_status_t query_status =
            vmm_address_space_query(
                space,
                virtual_page,
                &mapping
            );

        if (query_status == VMM_STATUS_NOT_MAPPED) {
            return USER_MEMORY_STATUS_NOT_MAPPED;
        }

        if (query_status != VMM_STATUS_OK) {
            return USER_MEMORY_STATUS_ADDRESS_SPACE_ERROR;
        }

        /*
         * A present mapping is not sufficient.
         * It must be effectively user accessible.
         */
        if (!mapping.user_accessible) {
            return USER_MEMORY_STATUS_NOT_USER_ACCESSIBLE;
        }

        /*
         * Writes additionally require effective write permission.
         */
        if ((access & USER_MEMORY_ACCESS_WRITE) &&
            !mapping.writable) {

            return USER_MEMORY_STATUS_NOT_WRITABLE;
        }

        if (page_aliases != NULL) {

            /*
             * vmm_address_space_query returns the translated
             * physical address, including the virtual offset.
             *
             * virtual_page is 4 KiB aligned, so align the
             * physical result down to the backing 4 KiB frame.
             *
             * This works for 4 KiB, 2 MiB and 1 GiB mappings.
             */
            uint64_t physical_page =
                mapping.physical_address &
                ~(PMM_PAGE_SIZE - 1ULL);

            /*
             * HUA's current PMM conversion API intentionally
             * supports PMM-owned frames only.
             *
             * Unsupported backing is rejected rather than
             * constructing an unchecked HHDM pointer.
             */
            void *alias =
                pmm_frame_to_virt(physical_page);

            if (alias == NULL) {
                return USER_MEMORY_STATUS_UNSUPPORTED_BACKING;
            }

            page_aliases[index] = alias;
        }
    }

    if (page_count_out != NULL) {
        *page_count_out = page_count;
    }

    return USER_MEMORY_STATUS_OK;
}


/*
 * Validate a user range without copying.
 */
user_memory_status_t user_range_validate(
    vmm_address_space_t *space,
    uint64_t user_address,
    size_t length,
    user_memory_access_t access
) {

    if (space == NULL ||
        !user_memory_access_is_valid(access)) {

        return USER_MEMORY_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Zero-length operations do not inspect the supplied
     * virtual address.
     */
    if (length == 0) {
        return USER_MEMORY_STATUS_OK;
    }

    struct user_memory_context context = {0};

    user_memory_status_t status =
        user_memory_context_begin(
            space,
            &context
        );

    if (status != USER_MEMORY_STATUS_OK) {
        return status;
    }

    status = user_memory_preflight(
        space,
        user_address,
        length,
        access,
        NULL,
        NULL
    );

    return user_memory_context_finish(
        &context,
        status
    );
}


/*
 * Copy bytes using validated HHDM aliases.
 *
 * The actual user virtual address is never dereferenced.
 */
static void user_memory_copy_bytes(
    uint8_t *kernel_destination,
    const uint8_t *kernel_source,
    uint64_t user_address,
    size_t length,
    void **page_aliases,
    bool copy_to_user
) {

    uint64_t first_page =
        user_address & ~(PMM_PAGE_SIZE - 1ULL);

    size_t completed = 0;

    while (completed < length) {

        uint64_t current_address =
            user_address + (uint64_t)completed;

        uint64_t current_page =
            current_address & ~(PMM_PAGE_SIZE - 1ULL);

        size_t page_index =
            (size_t)((current_page - first_page) /
                     PMM_PAGE_SIZE);

        size_t page_offset =
            (size_t)(current_address &
                     (PMM_PAGE_SIZE - 1ULL));

        size_t available =
            (size_t)PMM_PAGE_SIZE - page_offset;

        size_t remaining =
            length - completed;

        size_t chunk =
            remaining < available
                ? remaining
                : available;

        volatile uint8_t *user_alias =
            (volatile uint8_t *)page_aliases[page_index];

        user_alias += page_offset;

        if (copy_to_user) {

            for (size_t i = 0; i < chunk; i++) {

                user_alias[i] =
                    kernel_source[completed + i];
            }

        } else {

            for (size_t i = 0; i < chunk; i++) {

                kernel_destination[completed + i] =
                    user_alias[i];
            }
        }

        completed += chunk;
    }
}


/*
 * Copy from a user process into kernel memory.
 */
user_memory_status_t copy_from_user(
    vmm_address_space_t *space,
    void *kernel_destination,
    uint64_t user_source,
    size_t length
) {

    if (space == NULL) {
        return USER_MEMORY_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Zero-length copy is a no-op.
     */
    if (length == 0) {
        return USER_MEMORY_STATUS_OK;
    }

    if (kernel_destination == NULL) {
        return USER_MEMORY_STATUS_INVALID_ARGUMENT;
    }

    uintptr_t kernel_start =
        (uintptr_t)kernel_destination;

    if (kernel_start >
        UINTPTR_MAX - (uintptr_t)(length - 1U)) {

        return USER_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    struct user_memory_context context = {0};

    user_memory_status_t status =
        user_memory_context_begin(
            space,
            &context
        );

    if (status != USER_MEMORY_STATUS_OK) {
        return status;
    }

    void *page_aliases[USER_MEMORY_MAX_PAGES] = {0};

    status = user_memory_preflight(
        space,
        user_source,
        length,
        USER_MEMORY_ACCESS_READ,
        page_aliases,
        NULL
    );

    /*
     * Do not change the kernel destination until every
     * user page and backing frame has been validated.
     */
    if (status == USER_MEMORY_STATUS_OK) {

        user_memory_copy_bytes(
            (uint8_t *)kernel_destination,
            NULL,
            user_source,
            length,
            page_aliases,
            false
        );
    }

    return user_memory_context_finish(
        &context,
        status
    );
}


/*
 * Copy from kernel memory into a user process.
 */
user_memory_status_t copy_to_user(
    vmm_address_space_t *space,
    uint64_t user_destination,
    const void *kernel_source,
    size_t length
) {

    if (space == NULL) {
        return USER_MEMORY_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Zero-length copy is a no-op.
     */
    if (length == 0) {
        return USER_MEMORY_STATUS_OK;
    }

    if (kernel_source == NULL) {
        return USER_MEMORY_STATUS_INVALID_ARGUMENT;
    }

    uintptr_t kernel_start =
        (uintptr_t)kernel_source;

    if (kernel_start >
        UINTPTR_MAX - (uintptr_t)(length - 1U)) {

        return USER_MEMORY_STATUS_RANGE_OVERFLOW;
    }

    struct user_memory_context context = {0};

    user_memory_status_t status =
        user_memory_context_begin(
            space,
            &context
        );

    if (status != USER_MEMORY_STATUS_OK) {
        return status;
    }

    void *page_aliases[USER_MEMORY_MAX_PAGES] = {0};

    status = user_memory_preflight(
        space,
        user_destination,
        length,
        USER_MEMORY_ACCESS_WRITE,
        page_aliases,
        NULL
    );

    /*
     * Do not modify the user's memory until every page
     * has passed permission and backing validation.
     */
    if (status == USER_MEMORY_STATUS_OK) {

        user_memory_copy_bytes(
            NULL,
            (const uint8_t *)kernel_source,
            user_destination,
            length,
            page_aliases,
            true
        );
    }

    return user_memory_context_finish(
        &context,
        status
    );
}
