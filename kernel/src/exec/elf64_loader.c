#include <stddef.h>
#include <stdint.h>

#include "exec/elf64_loader.h"

#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#include "scheduler/thread.h"
#include "scheduler/scheduler.h"

#include "arch/x86_64/ring3.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/syscall.h"


/*
 * Maximum temporary page-record capacity.
 *
 * This matches the parser's existing image-page limit.
 */
#define ELF64_LOADER_PAGE_CAPACITY ELF64_MAX_IMAGE_PAGES


/*
 * Check whether two pointer ranges overlap.
 *
 * An overflowing range is treated as overlapping and rejected.
 */
static bool elf64_loader_ranges_overlap(
    const void *first,
    uint64_t first_size,
    const void *second,
    uint64_t second_size
) {
    if (first_size == 0 || second_size == 0) {
        return false;
    }

    if (first == NULL || second == NULL) {
        return true;
    }

    uintptr_t first_start = (uintptr_t)first;
    uintptr_t second_start = (uintptr_t)second;

    if (first_size >
        (uint64_t)(UINTPTR_MAX - first_start)) {

        return true;
    }

    if (second_size >
        (uint64_t)(UINTPTR_MAX - second_start)) {

        return true;
    }

    uintptr_t first_end =
        first_start + (uintptr_t)first_size;

    uintptr_t second_end =
        second_start + (uintptr_t)second_size;

    return first_start < second_end &&
        second_start < first_end;
}


/*
 * Check that every virtual page required by the ELF image
 * is currently unmapped in the target address space.
 *
 * Segment page ranges may share a page, so a page can be queried
 * more than once. This is harmless and avoids another large
 * temporary page-plan allocation.
 *
 * This is a preflight check, not a permanent reservation. VMM
 * still performs the authoritative collision check during mapping.
 */
static elf64_loader_status_t elf64_loader_preflight_mappings(
    vmm_address_space_t *address_space,
    const elf64_image_t *image
) {
    if (address_space == NULL ||
        image == NULL) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        const elf64_load_segment_t *segment =
            &image->segments[i];

        for (uint64_t virtual_address =
                 segment->page_start;
             virtual_address < segment->page_end;
             virtual_address += ELF64_PAGE_SIZE) {

            vmm_mapping_info_t mapping = {0};

            vmm_status_t status =
                vmm_address_space_query(
                    address_space,
                    virtual_address,
                    &mapping
                );

            if (status == VMM_STATUS_NOT_MAPPED) {
                continue;
            }

            if (status == VMM_STATUS_OK) {
                return ELF64_LOADER_STATUS_ADDRESS_CONFLICT;
            }

            return ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR;
        }
    }

    return ELF64_LOADER_STATUS_OK;
}

/*
 * Swap two process-owned image-page records.
 */
static void elf64_loader_page_swap(
    process_owned_page_t *a,
    process_owned_page_t *b
) {
    process_owned_page_t temporary = *a;

    *a = *b;
    *b = temporary;
}


/*
 * Return the sort key for one ownership record.
 *
 * A single heapsort implementation can therefore validate
 * either virtual-address uniqueness or physical-frame uniqueness.
 */
static uint64_t elf64_loader_page_sort_key(
    const process_owned_page_t *page,
    bool by_virtual_address
) {
    return by_virtual_address
        ? page->virtual_address
        : page->physical_address;
}


/*
 * Maintain a max heap according to the selected address key.
 */
static void elf64_loader_sift_down(
    process_owned_page_t *pages,
    uint64_t root,
    uint64_t heap_size,
    bool by_virtual_address
) {
    while (root < heap_size / 2ULL) {

        uint64_t child =
            root * 2ULL + 1ULL;

        if (child + 1ULL < heap_size &&
            elf64_loader_page_sort_key(
                &pages[child],
                by_virtual_address
            ) <
            elf64_loader_page_sort_key(
                &pages[child + 1ULL],
                by_virtual_address
            )) {

            child++;
        }

        if (elf64_loader_page_sort_key(
                &pages[root],
                by_virtual_address
            ) >=
            elf64_loader_page_sort_key(
                &pages[child],
                by_virtual_address
            )) {

            break;
        }

        elf64_loader_page_swap(
            &pages[root],
            &pages[child]
        );

        root = child;
    }
}


/*
 * Sort ownership records by one address field and reject duplicates.
 *
 * Heapsort:
 *     - O(n log n) worst-case time.
 *     - O(1) additional storage.
 *     - No recursive calls.
 *
 * Strict ordering rejects duplicate addresses.
 */
static bool elf64_loader_sort_and_validate(
    process_owned_page_t *pages,
    uint64_t count,
    bool by_virtual_address
) {
    if (pages == NULL || count == 0) {
        return false;
    }

    /*
     * Construct a max heap.
     */
    for (uint64_t start = count / 2ULL;
         start > 0;) {

        start--;

        elf64_loader_sift_down(
            pages,
            start,
            count,
            by_virtual_address
        );
    }

    /*
     * Extract the maximum element repeatedly.
     *
     * The resulting array is sorted in ascending order.
     */
    for (uint64_t end = count;
         end > 1ULL;) {

        elf64_loader_page_swap(
            &pages[0],
            &pages[end - 1ULL]
        );

        end--;

        elf64_loader_sift_down(
            pages,
            0,
            end,
            by_virtual_address
        );
    }

    /*
     * Require strict ascending order.
     * Equality means a duplicate address.
     */
    for (uint64_t i = 1;
         i < count;
         i++) {

        uint64_t previous =
            elf64_loader_page_sort_key(
                &pages[i - 1ULL],
                by_virtual_address
            );

        uint64_t current =
            elf64_loader_page_sort_key(
                &pages[i],
                by_virtual_address
            );

        if (previous >= current) {
            return false;
        }
    }

    return true;
}

/*
 * Convert prepared ELF pages into process-owned records.
 *
 * IMPORTANT:
 *
 * ELF permission encodings:
 *     PF_X = 0x1
 *     PF_W = 0x2
 *     PF_R = 0x4
 *
 * Process permission encodings:
 *     READ    = 0x1
 *     WRITE   = 0x2
 *     EXECUTE = 0x4
 *
 * These encodings differ. Translate them explicitly.
 */
static bool elf64_loader_populate_page_set(
    const elf64_prepared_page_t *prepared_pages,
    uint64_t prepared_count,
    process_owned_page_set_t *page_set
) {
    if (prepared_pages == NULL ||
        page_set == NULL ||
        prepared_count == 0 ||
        prepared_count > ELF64_LOADER_PAGE_CAPACITY) {

        return false;
    }

    if (page_set->attached ||
        page_set->cleanup_in_progress ||
        page_set->next != NULL) {

        return false;
    }

    const uint32_t allowed_elf_flags =
        ELF64_PF_R |
        ELF64_PF_W |
        ELF64_PF_X;

    /*
     * Convert every prepared record without assuming its input order.
     *
     * Normal preparation returns virtual-address order.
     * A cleanup-failure recovery path may return physical-address
     * order because elf64_release_prepared_pages() sorts in place.
     */
    for (uint64_t i = 0;
         i < prepared_count;
         i++) {

        const elf64_prepared_page_t *prepared =
            &prepared_pages[i];

        if (prepared->reserved != 0 ||
            prepared->virtual_address <
                ELF64_USER_MIN_ADDRESS ||
            prepared->virtual_address >=
                ELF64_USER_ADDRESS_LIMIT ||
            (prepared->virtual_address &
                ELF64_PAGE_MASK) != 0 ||
            prepared->physical_address == 0 ||
            (prepared->physical_address &
                ELF64_PAGE_MASK) != 0) {

            return false;
        }

        if ((prepared->segment_flags &
                ~allowed_elf_flags) != 0 ||
            !(prepared->segment_flags & ELF64_PF_R) ||
            ((prepared->segment_flags & ELF64_PF_W) &&
             (prepared->segment_flags & ELF64_PF_X))) {

            return false;
        }

        /*
         * PMM must recognize every physical frame as allocated.
         */
        if (pmm_frame_to_virt(
                prepared->physical_address
            ) == NULL) {

            return false;
        }

        process_owned_page_t *page =
            &page_set->pages[i];

        page->virtual_address =
            prepared->virtual_address;

        page->physical_address =
            prepared->physical_address;

        page->unexpected_physical_address = 0;

        page->permissions = 0;

        /*
         * Translate permission bits explicitly.
         *
         * ELF PF_R = 0x4, process READ = 0x1.
         * ELF PF_W = 0x2, process WRITE = 0x2.
         * ELF PF_X = 0x1, process EXECUTE = 0x4.
         */
        if (prepared->segment_flags & ELF64_PF_R) {
            page->permissions |= PROCESS_IMAGE_PAGE_READ;
        }

        if (prepared->segment_flags & ELF64_PF_W) {
            page->permissions |= PROCESS_IMAGE_PAGE_WRITE;
        }

        if (prepared->segment_flags & ELF64_PF_X) {
            page->permissions |= PROCESS_IMAGE_PAGE_EXECUTE;
        }

        page->frame_owned = true;
        page->mapped = false;
        page->quarantined = false;
    }

    page_set->page_count = prepared_count;

    /*
     * First sort by virtual address.
     *
     * This independently rejects duplicate virtual mappings,
     * regardless of the input record ordering.
     */
    if (!elf64_loader_sort_and_validate(
            page_set->pages,
            page_set->page_count,
            true
        )) {

        return false;
    }

    /*
     * Then sort by physical address.
     *
     * process_image_load_attach_pages() requires strictly
     * increasing physical addresses. This also rejects
     * duplicate ownership of the same physical frame.
     */
    if (!elf64_loader_sort_and_validate(
            page_set->pages,
            page_set->page_count,
            false
        )) {

        return false;
    }

    return true;
}


/*
 * Verify the actual page tables after all mappings have been
 * installed and before the process is marked image-loaded.
 *
 * Software mapped flags alone are not sufficient evidence.
 */
static bool elf64_loader_verify_mappings(
    vmm_address_space_t *address_space,
    const process_owned_page_set_t *page_set
) {
    if (address_space == NULL ||
        page_set == NULL ||
        page_set->page_count == 0) {

        return false;
    }

    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        const process_owned_page_t *page =
            &page_set->pages[i];

        vmm_mapping_info_t mapping = {0};

        vmm_status_t status =
            vmm_address_space_query(
                address_space,
                page->virtual_address,
                &mapping
            );

        if (status != VMM_STATUS_OK ||
            mapping.page_size != ELF64_PAGE_SIZE ||
            mapping.huge_page ||
            mapping.physical_address !=
                page->physical_address ||
            !mapping.user_accessible) {

            return false;
        }

        bool expected_writable =
            (page->permissions &
                PROCESS_IMAGE_PAGE_WRITE) != 0;

        bool expected_executable =
            (page->permissions &
                PROCESS_IMAGE_PAGE_EXECUTE) != 0;

        if (mapping.writable != expected_writable ||
            mapping.executable != expected_executable) {

            return false;
        }
    }

    return true;
}


/*
 * End a failed image transaction after page ownership has
 * transferred to the process.
 *
 * If release fails, the page set remains attached to the process.
 * It must not be freed by the loader.
 */
static bool elf64_loader_rollback_attached(
    process_t *process,
    process_owned_page_set_t *page_set,
    bool *loading,
    bool *attached
) {
    if (process == NULL ||
        page_set == NULL ||
        loading == NULL ||
        attached == NULL) {

        return false;
    }

    if (*loading) {

        if (!process_image_load_finish(
                process,
                false
            )) {

            return false;
        }

        *loading = false;
    }

    if (*attached) {

        if (!process_release_owned_pages(
                process,
                page_set
            )) {

            /*
             * Ownership records remain process-owned.
             * Do not free page_set here.
             */
            return false;
        }

        *attached = false;
    }

    return true;
}


/*
 * Loader implementation.
 */
elf64_loader_status_t elf64_load_process(
    process_t *process,
    const void *file_data,
    uint64_t file_size,
    uint64_t *entry_out,
    elf64_status_t *elf_status_out
) {
    if (process == NULL ||
        file_data == NULL ||
        file_size == 0 ||
        entry_out == NULL) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Output pointers must not alias the input executable.
     * Otherwise reporting success could modify the file bytes.
     */
    if (elf64_loader_ranges_overlap(
            file_data,
            file_size,
            entry_out,
            sizeof(*entry_out)
        )) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    if (elf_status_out != NULL) {

        if (elf64_loader_ranges_overlap(
                file_data,
                file_size,
                elf_status_out,
                sizeof(*elf_status_out)
            ) ||
            elf64_loader_ranges_overlap(
                entry_out,
                sizeof(*entry_out),
                elf_status_out,
                sizeof(*elf_status_out)
            )) {

            return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
        }
    }

    if (elf_status_out != NULL) {
        *elf_status_out = ELF64_STATUS_OK;
    }

    /*
     * Parse before reserving the process.
     *
     * Invalid ELF files therefore do not change process state.
     */
    elf64_image_t image = {0};

    elf64_status_t elf_status =
        elf64_parse(
            file_data,
            file_size,
            &image
        );

    if (elf_status_out != NULL) {
        *elf_status_out = elf_status;
    }

    if (elf_status != ELF64_STATUS_OK) {
        return ELF64_LOADER_STATUS_PARSE_FAILED;
    }

    /*
     * Allocate storage for the worst permitted image size.
     *
     * The page-set allocation happens before physical frames
     * are allocated. This ensures metadata capacity already
     * exists when ownership needs to transfer to the process.
     *
     * The worst-case temporary storage is approximately 1 MiB
     * for the current 16,384-page image limit.
     */
    if ((uint64_t)ELF64_LOADER_PAGE_CAPACITY >
            (uint64_t)(SIZE_MAX /
                sizeof(elf64_prepared_page_t)) ||
        (uint64_t)ELF64_LOADER_PAGE_CAPACITY >
            (uint64_t)((SIZE_MAX -
                sizeof(process_owned_page_set_t)) /
                sizeof(process_owned_page_t))) {

        return ELF64_LOADER_STATUS_OUT_OF_MEMORY;
    }

    size_t prepared_storage_size =
        (size_t)ELF64_LOADER_PAGE_CAPACITY *
        sizeof(elf64_prepared_page_t);

    size_t page_set_storage_size =
        sizeof(process_owned_page_set_t) +
        (size_t)ELF64_LOADER_PAGE_CAPACITY *
            sizeof(process_owned_page_t);

    elf64_prepared_page_t *prepared_pages =
        (elf64_prepared_page_t *)kmalloc(
            prepared_storage_size
        );

    if (prepared_pages == NULL) {
        return ELF64_LOADER_STATUS_OUT_OF_MEMORY;
    }

    process_owned_page_set_t *page_set =
        (process_owned_page_set_t *)kcalloc(
            1,
            page_set_storage_size
        );

    if (page_set == NULL) {
        kfree(prepared_pages);
        return ELF64_LOADER_STATUS_OUT_OF_MEMORY;
    }

    uint64_t prepared_count = 0;

    bool loading = false;
    bool attached = false;

    vmm_address_space_t *address_space = NULL;

    elf64_loader_status_t result =
        ELF64_LOADER_STATUS_PROCESS_STATE;

    /*
     * Reserve the target process for this load transaction.
     */
    if (!process_image_load_begin(
            process,
            &address_space
        )) {

        goto cleanup_unattached;
    }

    loading = true;

    if (address_space == NULL) {

        result =
            ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR;

        goto cleanup_unattached;
    }

    /*
     * Reject existing virtual mappings before allocating
     * image frames or transferring any ownership.
     */
    result = elf64_loader_preflight_mappings(
        address_space,
        &image
    );

    if (result != ELF64_LOADER_STATUS_OK) {
        goto cleanup_unattached;
    }

    /*
     * Allocate and populate the physical image pages.
     *
     * A normal failure leaves prepared_count unchanged.
     * CLEANUP_FAILED can return unreleased frames in the array.
     */
    elf_status =
        elf64_prepare_pages(
            &image,
            prepared_pages,
            ELF64_LOADER_PAGE_CAPACITY,
            &prepared_count
        );

    if (elf_status_out != NULL) {
        *elf_status_out = elf_status;
    }

    if (elf_status != ELF64_STATUS_OK) {

        result =
            elf_status == ELF64_STATUS_OUT_OF_MEMORY
                ? ELF64_LOADER_STATUS_OUT_OF_MEMORY
                : ELF64_LOADER_STATUS_PREPARATION_FAILED;

        /*
         * If preparation itself could not release every frame,
         * the remaining records are still adopted below.
         */
        if (elf_status != ELF64_STATUS_CLEANUP_FAILED ||
            prepared_count == 0) {

            goto cleanup_unattached;
        }
    }

    if (prepared_count == 0) {

        result =
            ELF64_LOADER_STATUS_PREPARATION_FAILED;

        goto cleanup_unattached;
    }

    /*
     * Convert ELF permissions and create process ownership records.
     *
     * The conversion validates the prepared records again and
     * sorts them by physical address before ownership transfer.
     */
    if (!elf64_loader_populate_page_set(
            prepared_pages,
            prepared_count,
            page_set
        )) {

        result =
            ELF64_LOADER_STATUS_OWNERSHIP_FAILED;

        goto cleanup_unattached;
    }

    /*
     * Ownership transfers only when this operation succeeds.
     */
    if (!process_image_load_attach_pages(
            process,
            page_set
        )) {

        result =
            ELF64_LOADER_STATUS_OWNERSHIP_FAILED;

        goto cleanup_unattached;
    }

    attached = true;

    /*
     * The process now owns the frames and page-set metadata.
     * The prepared-page array contains only redundant copies.
     */
    prepared_count = 0;

    /*
     * A partial preparation cleanup failure has been safely
     * adopted by the process. Do not attempt to map an incomplete
     * image; end the transaction and release those pages.
     */
    if (elf_status != ELF64_STATUS_OK) {
        goto cleanup_attached;
    }

    /*
     * Map every process-owned page.
     *
     * The records are sorted by physical address. Mapping order
     * does not depend on virtual-address order.
     */
    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        if (!process_image_load_map_page(
                process,
                page_set,
                i
            )) {

            result =
                ELF64_LOADER_STATUS_MAPPING_FAILED;

            goto cleanup_attached;
        }
    }

    /*
     * Query actual page tables and verify physical identity
     * and effective user/write/execute permissions.
     */
    if (!elf64_loader_verify_mappings(
            address_space,
            page_set
        )) {

        result =
            ELF64_LOADER_STATUS_MAPPING_FAILED;

        goto cleanup_attached;
    }

    /*
     * Commit the completed image.
     *
     * process_image_load_finish() clears image_loading when its
     * reservation is valid, including when its final page-set
     * validation rejects the image.
     */
    bool finish_ok =
        process_image_load_finish(
            process,
            true
        );

    loading = false;

    if (!finish_ok) {

        result =
            ELF64_LOADER_STATUS_OWNERSHIP_FAILED;

        goto cleanup_attached;
    }

    /*
     * Publish the entry point only after the process manager
     * has accepted the complete image.
     */
    *entry_out = image.entry;

    result = ELF64_LOADER_STATUS_OK;

    goto cleanup_unattached;


cleanup_attached:

    /*
     * End the image-load reservation before asking the process
     * manager to unmap and release its owned pages.
     */
    if (!elf64_loader_rollback_attached(
            process,
            page_set,
            &loading,
            &attached
        )) {

        result =
            ELF64_LOADER_STATUS_CLEANUP_FAILED;

    } else {

        /*
         * Successful process cleanup already freed page_set.
         */
        page_set = NULL;
    }

    goto cleanup_unattached;


cleanup_unattached:

    /*
     * If frames remain caller-owned, first try to adopt them
     * into the process so its retryable cleanup path can own them.
     *
     * This also handles a transient failure during the first
     * attach attempt.
     */
    if (!attached && prepared_count != 0) {

        bool adopted = false;

        if (elf64_loader_populate_page_set(
                prepared_pages,
                prepared_count,
                page_set
            ) &&
            process_image_load_attach_pages(
                process,
                page_set
            )) {

            adopted = true;
            attached = true;
            prepared_count = 0;
        }

        if (adopted) {

            if (!elf64_loader_rollback_attached(
                    process,
                    page_set,
                    &loading,
                    &attached
                )) {

                result =
                    ELF64_LOADER_STATUS_CLEANUP_FAILED;

            } else {

                page_set = NULL;
            }
        }
    }

    /*
     * If ownership did not transfer, release any remaining
     * prepared frames directly through the ELF preparation API.
     */
    if (!attached && prepared_count != 0) {

        elf64_status_t release_status =
            elf64_release_prepared_pages(
                prepared_pages,
                &prepared_count
            );

        if (release_status != ELF64_STATUS_OK ||
            prepared_count != 0) {

            /*
             * release_prepared_pages() can compact unreleased
             * records at the beginning of prepared_pages.
             *
             * Try to transfer those remaining frames to the
             * process so its cleanup mechanism can retry them.
             */
            bool adopted_after_release = false;

            if (prepared_count != 0 &&
                elf64_loader_populate_page_set(
                    prepared_pages,
                    prepared_count,
                    page_set
                ) &&
                process_image_load_attach_pages(
                    process,
                    page_set
                )) {

                adopted_after_release = true;
                attached = true;
                prepared_count = 0;
            }

            if (adopted_after_release) {

                if (!elf64_loader_rollback_attached(
                        process,
                        page_set,
                        &loading,
                        &attached
                    )) {

                    result =
                        ELF64_LOADER_STATUS_CLEANUP_FAILED;

                } else {

                    page_set = NULL;
                }

            } else {

                result =
                    ELF64_LOADER_STATUS_CLEANUP_FAILED;
            }
        }
    }

    /*
     * End the reservation when no page set was attached.
     */
    if (loading && !attached) {

        if (process_image_load_finish(
                process,
                false
            )) {

            loading = false;

        } else {

            result =
                ELF64_LOADER_STATUS_CLEANUP_FAILED;
        }
    }

    /*
     * An attached page set is either:
     *     - already released and detached, or
     *     - still owned by the process after cleanup failure.
     *
     * Never kfree an attached set from the loader.
     */
    if (!attached && page_set != NULL) {
        kfree(page_set);
    }

    /*
     * Preserve the prepared records if PMM could not release
     * their frames. This is an invariant-failure fallback;
     * the normal path always reaches prepared_count == 0.
     */
    if (prepared_count == 0) {
        kfree(prepared_pages);
    }

    return result;
}

/*
 * Fail closed if physical frames have been allocated but cannot
 * be transferred to the process manager or released.
 *
 * This represents an internal ownership invariant violation,
 * not an ordinary allocation failure.
 */
static _Noreturn void elf64_loader_ownership_fail_stop(void) {

    asm volatile ("cli" ::: "memory");

    for (;;) {
        asm volatile ("hlt" ::: "memory");
    }
}


/*
 * Sort the small initial-stack page set by physical address.
 *
 * process_image_load_attach_pages() requires strictly increasing
 * physical addresses. Never assume PMM allocation order.
 */
static bool elf64_loader_sort_stack_pages(
    process_owned_page_set_t *page_set
) {

    if (page_set == NULL ||
        page_set->page_count == 0) {

        return false;
    }

    for (uint64_t i = 1;
         i < page_set->page_count;
         i++) {

        process_owned_page_t value =
            page_set->pages[i];

        uint64_t position = i;

        while (position > 0 &&
               page_set->pages[position - 1ULL].physical_address >
                   value.physical_address) {

            page_set->pages[position] =
                page_set->pages[position - 1ULL];

            position--;
        }

        page_set->pages[position] = value;
    }

    for (uint64_t i = 1;
         i < page_set->page_count;
         i++) {

        if (page_set->pages[i - 1ULL].physical_address >=
            page_set->pages[i].physical_address) {

            return false;
        }
    }

    return true;
}


/*
 * Verify that the reserved guard page and every initial stack page
 * are currently unmapped.
 *
 * The ELF image has already been loaded, so this also detects
 * collisions between the executable and its reserved stack region.
 */
static elf64_loader_status_t elf64_loader_preflight_user_stack(
    vmm_address_space_t *address_space
) {

    if (address_space == NULL ||
        ELF64_USER_STACK_GUARD < ELF64_USER_MIN_ADDRESS ||
        ELF64_USER_STACK_TOP >= ELF64_USER_ADDRESS_LIMIT ||
        ELF64_USER_STACK_BASE >= ELF64_USER_STACK_TOP ||
        ELF64_USER_STACK_SIZE !=
            ELF64_USER_STACK_PAGE_COUNT * ELF64_PAGE_SIZE) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    for (uint64_t address = ELF64_USER_STACK_GUARD;
         address < ELF64_USER_STACK_TOP;
         address += ELF64_PAGE_SIZE) {

        vmm_mapping_info_t mapping = {0};

        vmm_status_t status =
            vmm_address_space_query(
                address_space,
                address,
                &mapping
            );

        if (status == VMM_STATUS_OK) {
            return ELF64_LOADER_STATUS_ADDRESS_CONFLICT;
        }

        if (status != VMM_STATUS_NOT_MAPPED) {
            return ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR;
        }
    }

    return ELF64_LOADER_STATUS_OK;
}


/*
 * Abort the initial-stack transaction.
 *
 * An attached page set is released only by the process manager.
 * An unattached page set remains caller-owned and is released here.
 */
static elf64_loader_status_t elf64_loader_abort_user_stack(
    process_t *process,
    process_owned_page_set_t *page_set,
    bool attached,
    elf64_loader_status_t original_status
) {

    if (process == NULL) {
        return ELF64_LOADER_STATUS_CLEANUP_FAILED;
    }

    /*
     * Close the reservation before releasing an attached page set.
     */
    if (!process_user_start_finish(
            process,
            false,
            ELF64_USER_STACK_BASE,
            ELF64_USER_STACK_TOP
        )) {

        return ELF64_LOADER_STATUS_CLEANUP_FAILED;
    }

    if (attached) {

        /*
         * The process owns page_set. A failure retains the
         * page set and remaining frames in the process.
         */
        if (page_set == NULL ||
            !process_release_owned_pages(
                process,
                page_set
            )) {

            return ELF64_LOADER_STATUS_CLEANUP_FAILED;
        }

        page_set = NULL;

    } else if (page_set != NULL) {

        /*
         * Ownership never transferred. Release each frame directly.
         *
         * If a PMM release unexpectedly fails, do not discard the
         * only ownership records for that frame.
         */
        for (uint64_t i = 0;
             i < page_set->page_count;
             i++) {

            process_owned_page_t *page =
                &page_set->pages[i];

            if (!page->frame_owned) {
                continue;
            }

            if (!pmm_free_frame(page->physical_address)) {
                elf64_loader_ownership_fail_stop();
            }

            page->frame_owned = false;
        }

        kfree(page_set);
        page_set = NULL;
    }

    /*
     * Retry is allowed only after no records from this stack
     * transaction remain attached to the process.
     */
    if (!process_user_start_reset_failed(process)) {
        return ELF64_LOADER_STATUS_CLEANUP_FAILED;
    }

    return original_status;
}


/*
 * Prepare a 64 KiB initial user stack with one guard page.
 */
elf64_loader_status_t elf64_prepare_user_stack(
    process_t *process,
    uint64_t *user_rsp_out
) {

    if (process == NULL ||
        user_rsp_out == NULL) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_user_start_begin(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        return ELF64_LOADER_STATUS_PROCESS_STATE;
    }

    /*
     * No frames are allocated until the complete reserved
     * stack region has passed its collision check.
     */
    elf64_loader_status_t status =
        elf64_loader_preflight_user_stack(address_space);

    if (status != ELF64_LOADER_STATUS_OK) {

        return elf64_loader_abort_user_stack(
            process,
            NULL,
            false,
            status
        );
    }

    /*
     * Reserve all page metadata before allocating physical frames.
     */
    if (ELF64_USER_STACK_PAGE_COUNT >
        (SIZE_MAX - sizeof(process_owned_page_set_t)) /
            sizeof(process_owned_page_t)) {

        return elf64_loader_abort_user_stack(
            process,
            NULL,
            false,
            ELF64_LOADER_STATUS_OUT_OF_MEMORY
        );
    }

    size_t page_set_size =
        sizeof(process_owned_page_set_t) +
        (size_t)ELF64_USER_STACK_PAGE_COUNT *
            sizeof(process_owned_page_t);

    process_owned_page_set_t *page_set =
        (process_owned_page_set_t *)kcalloc(
            1,
            page_set_size
        );

    if (page_set == NULL) {

        return elf64_loader_abort_user_stack(
            process,
            NULL,
            false,
            ELF64_LOADER_STATUS_OUT_OF_MEMORY
        );
    }

    uint64_t last_stack_physical = 0;

    bool allocation_complete = true;

    /*
     * Allocate and zero every user-stack frame.
     *
     * The stack is writable and non-executable.
     */
    for (uint64_t i = 0;
         i < ELF64_USER_STACK_PAGE_COUNT;
         i++) {

        uint64_t physical =
            pmm_alloc_frame();

        if (physical == 0) {
            allocation_complete = false;
            break;
        }

        void *virtual_alias =
            pmm_frame_to_virt(physical);

        if (virtual_alias == NULL) {

            if (!pmm_free_frame(physical)) {
                elf64_loader_ownership_fail_stop();
            }

            allocation_complete = false;
            break;
        }

        uint8_t *bytes =
            (uint8_t *)virtual_alias;

        for (uint64_t j = 0;
             j < ELF64_PAGE_SIZE;
             j++) {

            bytes[j] = 0;
        }

        process_owned_page_t *page =
            &page_set->pages[page_set->page_count];

        page->virtual_address =
            ELF64_USER_STACK_BASE +
            i * ELF64_PAGE_SIZE;

        page->physical_address = physical;

        page->unexpected_physical_address = 0;

        page->permissions =
            PROCESS_IMAGE_PAGE_READ |
            PROCESS_IMAGE_PAGE_WRITE;

        page->frame_owned = true;
        page->mapped = false;
        page->quarantined = false;

        page_set->page_count++;

        if (i == ELF64_USER_STACK_PAGE_COUNT - 1ULL) {
            last_stack_physical = physical;
        }
    }

    /*
     * Even a partially allocated stack is transferred into
     * process ownership before rollback.
     *
     * That gives the process manager responsibility for cleanup
     * and allows retry if PMM release temporarily fails.
     */
    if (page_set->page_count != 0 &&
        !elf64_loader_sort_stack_pages(page_set)) {

        /*
         * A partial page set with one page is valid. The sort
         * routine also accepts it; failure here means an invariant
         * violation such as duplicate physical-frame ownership.
         */
        elf64_loader_ownership_fail_stop();
    }

    if (page_set->page_count != 0) {

        if (!process_image_load_attach_pages(
                process,
                page_set
            )) {

            /*
             * The reservation and our records are still valid.
             * Since no mapping has been made, release the frames
             * directly if ownership transfer is rejected.
             */
            return elf64_loader_abort_user_stack(
                process,
                page_set,
                false,
                ELF64_LOADER_STATUS_OWNERSHIP_FAILED
            );
        }
    }

    bool attached =
        page_set->page_count != 0;

    if (!allocation_complete) {

        return elf64_loader_abort_user_stack(
            process,
            page_set,
            attached,
            ELF64_LOADER_STATUS_OUT_OF_MEMORY
        );
    }

    /*
     * Construct the initial user stack:
     *
     *   RSP +  0: argc = 0
     *   RSP +  8: argv[0] = NULL
     *   RSP + 16: envp[0] = NULL
     *   RSP + 24: AT_NULL
     *   RSP + 32: AT_NULL value
     *   RSP + 40: alignment padding
     *
     * RSP is 16-byte aligned.
     */
    const uint64_t initial_stack_words[6] = {
        0,
        0,
        0,
        0,
        0,
        0
    };

    if (last_stack_physical == 0) {

        return elf64_loader_abort_user_stack(
            process,
            page_set,
            attached,
            ELF64_LOADER_STATUS_OWNERSHIP_FAILED
        );
    }

    uint8_t *last_page =
        (uint8_t *)pmm_frame_to_virt(
            last_stack_physical
        );

    if (last_page == NULL) {
        return elf64_loader_abort_user_stack(
            process,
            page_set,
            attached,
            ELF64_LOADER_STATUS_OWNERSHIP_FAILED
        );
    }

    uint64_t user_rsp =
        ELF64_USER_STACK_TOP -
        ELF64_USER_INITIAL_STACK_SIZE;

    uint64_t stack_page_offset =
        ELF64_PAGE_SIZE -
        ELF64_USER_INITIAL_STACK_SIZE;

    uint64_t *initial_stack =
        (uint64_t *)(last_page + stack_page_offset);

    for (uint64_t i = 0; i < 6; i++) {
        initial_stack[i] = initial_stack_words[i];
    }

    /*
     * Map every stack page through the process ownership API.
     */
    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        if (!process_image_load_map_page(
                process,
                page_set,
                i
            )) {

            return elf64_loader_abort_user_stack(
                process,
                page_set,
                true,
                ELF64_LOADER_STATUS_MAPPING_FAILED
            );
        }
    }

    /*
     * The complete stack must have been allocated.
     */
    if (page_set->page_count !=
        ELF64_USER_STACK_PAGE_COUNT) {

        return elf64_loader_abort_user_stack(
            process,
            page_set,
            true,
            ELF64_LOADER_STATUS_OWNERSHIP_FAILED
        );
    }

    /*
     * Query the actual page tables independently.
     */
    for (uint64_t i = 0;
         i < page_set->page_count;
         i++) {

        const process_owned_page_t *page =
            &page_set->pages[i];

        vmm_mapping_info_t mapping = {0};

        if (vmm_address_space_query(
                address_space,
                page->virtual_address,
                &mapping
            ) != VMM_STATUS_OK ||
            mapping.page_size != ELF64_PAGE_SIZE ||
            mapping.physical_address != page->physical_address ||
            !mapping.user_accessible ||
            !mapping.writable ||
            mapping.executable) {

            return elf64_loader_abort_user_stack(
                process,
                page_set,
                true,
                ELF64_LOADER_STATUS_MAPPING_FAILED
            );
        }
    }

    /*
     * The process manager performs the final ownership and
     * guard-page validation before publishing stack readiness.
     */
    if (!process_user_start_finish(
            process,
            true,
            ELF64_USER_STACK_BASE,
            ELF64_USER_STACK_TOP
        )) {

        return elf64_loader_abort_user_stack(
            process,
            page_set,
            true,
            ELF64_LOADER_STATUS_OWNERSHIP_FAILED
        );
    }

    /*
     * Ownership has transferred completely to the process.
     * Do not touch page_set after this point.
     */
    *user_rsp_out = user_rsp;

    return ELF64_LOADER_STATUS_OK;
}

elf64_loader_status_t elf64_load_process_with_stack(
    process_t *process,
    const void *file_data,
    uint64_t file_size,
    uint64_t *entry_out,
    uint64_t *user_rsp_out,
    elf64_status_t *elf_status_out
) {

    if (process == NULL ||
        file_data == NULL ||
        file_size == 0 ||
        entry_out == NULL ||
        user_rsp_out == NULL) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    /*
     * All output ranges must be independent and must not overlap
     * the input executable.
     */
    if (elf64_loader_ranges_overlap(
            file_data,
            file_size,
            entry_out,
            sizeof(*entry_out)
        ) ||
        elf64_loader_ranges_overlap(
            file_data,
            file_size,
            user_rsp_out,
            sizeof(*user_rsp_out)
        ) ||
        elf64_loader_ranges_overlap(
            entry_out,
            sizeof(*entry_out),
            user_rsp_out,
            sizeof(*user_rsp_out)
        )) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    if (elf_status_out != NULL) {

        if (elf64_loader_ranges_overlap(
                file_data,
                file_size,
                elf_status_out,
                sizeof(*elf_status_out)
            ) ||
            elf64_loader_ranges_overlap(
                entry_out,
                sizeof(*entry_out),
                elf_status_out,
                sizeof(*elf_status_out)
            ) ||
            elf64_loader_ranges_overlap(
                user_rsp_out,
                sizeof(*user_rsp_out),
                elf_status_out,
                sizeof(*elf_status_out)
            )) {

            return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
        }
    }

    uint64_t local_entry = 0;
    uint64_t local_user_rsp = 0;

    elf64_status_t local_elf_status =
        ELF64_STATUS_OK;

    /*
     * First commit the ELF image using the existing, tested loader.
     */
    elf64_loader_status_t status =
        elf64_load_process(
            process,
            file_data,
            file_size,
            &local_entry,
            &local_elf_status
        );

    if (elf_status_out != NULL) {
        *elf_status_out = local_elf_status;
    }

    if (status != ELF64_LOADER_STATUS_OK) {
        return status;
    }

    /*
     * Then build and commit the initial user stack.
     *
     * The process remains NEW and has no attached threads throughout.
     */
    status = elf64_prepare_user_stack(
        process,
        &local_user_rsp
    );

    if (status != ELF64_LOADER_STATUS_OK) {
        return status;
    }

    /*
     * Publish outputs only after the image and initial stack
     * have both committed successfully.
     */
    *entry_out = local_entry;
    *user_rsp_out = local_user_rsp;

    return ELF64_LOADER_STATUS_OK;
}

struct elf64_initial_thread_context {

    uint64_t user_entry;

    uint64_t user_rsp;

};


/*
 * First kernel instruction path for a real user process.
 *
 * It never returns to the ordinary kernel-thread bootstrap.
 */
static _Noreturn void elf64_user_process_first_entry(
    void *argument
) {

    struct elf64_initial_thread_context *context =
        (struct elf64_initial_thread_context *)argument;

    if (context == NULL) {
        thread_exit(-1);
    }

    uint64_t user_entry = context->user_entry;
    uint64_t user_rsp = context->user_rsp;

    /*
     * The context is no longer needed after copying its values.
     * Releasing it here avoids making its lifetime depend on
     * the eventual user-process exit path.
     */
    kfree(context);

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING ||
        current->process == NULL ||
        current->address_space == NULL ||
        !current->stack_owned ||
        current->stack_base == NULL ||
        current->stack_size < THREAD_STACK_MIN_SIZE ||
        current->preempt_count != 0) {

        thread_exit(-1);
    }

    process_state_t process_state = PROCESS_STATE_NEW;
    uint32_t process_thread_count = 0;

    if (!process_get_info(
            current->process,
            NULL,
            &process_state,
            &process_thread_count,
            NULL,
            0
        ) ||
        process_state != PROCESS_STATE_RUNNING ||
        process_thread_count != 1 ||
        !process_image_is_loaded(current->process)) {

        thread_exit(-1);
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            current->process,
            &address_space
        ) ||
        address_space == NULL ||
        address_space != current->address_space) {

        thread_exit(-1);
    }

    uint64_t stack_base = 0;
    uint64_t stack_top = 0;

    if (!process_get_user_stack(
            current->process,
            &stack_base,
            &stack_top
        ) ||
        stack_top != ELF64_USER_STACK_TOP ||
        stack_base != ELF64_USER_STACK_BASE ||
        user_rsp !=
            stack_top - ELF64_USER_INITIAL_STACK_SIZE ||
        (user_rsp & 15ULL) != 0) {

        thread_exit(-1);
    }

    /*
     * Revalidate actual user permissions immediately before
     * the privilege transition.
     */
    vmm_mapping_info_t code_mapping = {0};
    vmm_mapping_info_t stack_mapping = {0};
    vmm_mapping_info_t guard_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            user_entry,
            &code_mapping
        ) != VMM_STATUS_OK ||
        !code_mapping.user_accessible ||
        code_mapping.writable ||
        !code_mapping.executable ||
        code_mapping.huge_page) {

        thread_exit(-1);
    }

    if (vmm_address_space_query(
            address_space,
            user_rsp - 1ULL,
            &stack_mapping
        ) != VMM_STATUS_OK ||
        !stack_mapping.user_accessible ||
        !stack_mapping.writable ||
        stack_mapping.executable ||
        stack_mapping.huge_page) {

        thread_exit(-1);
    }

    if (vmm_address_space_query(
            address_space,
            stack_base - ELF64_PAGE_SIZE,
            &guard_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        thread_exit(-1);
    }

    /*
     * The scheduler must have selected this thread's own kernel
     * stack for both hardware privilege transitions and SYSCALL.
     */
    uintptr_t rsp0 = 0;
    uintptr_t syscall_stack = 0;

    if (!gdt_get_kernel_stack(&rsp0) ||
        rsp0 != current->stack_top ||

        !arch_syscall_get_kernel_stack(&syscall_stack) ||
        syscall_stack != current->stack_top) {

        thread_exit(-1);
    }

    /*
     * Confirm that this thread is executing with its process CR3.
     */
    uint64_t expected_root = 0;

    if (!vmm_address_space_get_root_physical(
            address_space,
            &expected_root
        )) {

        thread_exit(-1);
    }

    uint64_t cr3 = 0;

    asm volatile (
        "mov %%cr3, %0"
        : "=r"(cr3)
        :
        : "memory"
    );

    if ((cr3 & 0x000FFFFFFFFFF000ULL) != expected_root) {
        thread_exit(-1);
    }

    /*
     * One-way, normal user-process transition.
     *
     * IRETQ enters CPL3 with IF=1. The process can now be
     * interrupted and preempted by HUA's ordinary scheduler.
     */
    arch_ring3_enter_user_preemptible_noreturn(
        user_entry,
        user_rsp
    );
}

elf64_loader_status_t elf64_start_process(
    process_t *process,
    uint64_t user_entry,
    uint64_t user_rsp
) {

    if (process == NULL ||
        user_entry == 0 ||
        user_entry < ELF64_USER_MIN_ADDRESS ||
        user_entry >= ELF64_USER_ADDRESS_LIMIT ||
        (user_rsp & 15ULL) != 0) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    if (!scheduler_is_initialized()) {
        return ELF64_LOADER_STATUS_SCHEDULER_FAILED;
    }

    if (!process_image_is_loaded(process)) {
        return ELF64_LOADER_STATUS_PROCESS_STATE;
    }

    process_state_t state = PROCESS_STATE_NEW;
    uint32_t thread_count = UINT32_MAX;

    if (!process_get_info(
            process,
            NULL,
            &state,
            &thread_count,
            NULL,
            0
        ) ||
        state != PROCESS_STATE_NEW ||
        thread_count != 0) {

        return ELF64_LOADER_STATUS_PROCESS_STATE;
    }

    uint64_t stack_base = 0;
    uint64_t stack_top = 0;

    if (!process_get_user_stack(
            process,
            &stack_base,
            &stack_top
        )) {

        return ELF64_LOADER_STATUS_PROCESS_STATE;
    }

    /*
     * HUA's initial user-entry ABI requires this exact initial
     * stack pointer. It points at argc and is 16-byte aligned.
     */
    if (user_rsp !=
            stack_top - ELF64_USER_INITIAL_STACK_SIZE ||
        user_rsp <= stack_base ||
        user_rsp >= stack_top) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        return ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR;
    }

    /*
     * Require a user-readable, executable, read-only entry mapping.
     */
    vmm_mapping_info_t code_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            user_entry,
            &code_mapping
        ) != VMM_STATUS_OK) {

        return ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR;
    }

    if (!code_mapping.user_accessible ||
        code_mapping.writable ||
        !code_mapping.executable) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Require a user-writable, NX stack mapping.
     */
    vmm_mapping_info_t stack_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            user_rsp - 1ULL,
            &stack_mapping
        ) != VMM_STATUS_OK) {

        return ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR;
    }

    if (!stack_mapping.user_accessible ||
        !stack_mapping.writable ||
        stack_mapping.executable) {

        return ELF64_LOADER_STATUS_INVALID_ARGUMENT;
    }

    /*
     * The guard page must remain absent.
     */
    vmm_mapping_info_t guard_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            stack_base - ELF64_PAGE_SIZE,
            &guard_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return ELF64_LOADER_STATUS_ADDRESS_CONFLICT;
    }

    /*
     * Allocate the small startup argument object before creating
     * the thread. It becomes owned by the new thread's entry path.
     */
    struct elf64_initial_thread_context *context =
        (struct elf64_initial_thread_context *)kmalloc(
            sizeof(struct elf64_initial_thread_context)
        );

    if (context == NULL) {
        return ELF64_LOADER_STATUS_OUT_OF_MEMORY;
    }

    context->user_entry = user_entry;
    context->user_rsp = user_rsp;

    /*
     * Create and attach the first process-owned thread.
     *
     * process_attach_thread() consumes the committed startup
     * permission and transitions the process to RUNNING.
     */
    struct thread *thread =
        thread_create_for_process(
            process,
            "user-main",
            elf64_user_process_first_entry,
            context,
            0
        );

    if (thread == NULL) {

        kfree(context);

        return ELF64_LOADER_STATUS_THREAD_CREATE_FAILED;
    }

    /*
     * Publish the thread to the run queue.
     *
     * After successful enqueue, the TCB may run and be reclaimed
     * immediately. Do not dereference thread after success.
     */
    if (!scheduler_enqueue_thread(0, thread)) {

        /*
         * Enqueue failure must leave the thread NEW and detached
         * from scheduler queues. Restore the process to NEW before
         * destroying the TCB.
         */
        if (!process_cancel_initial_thread(
                process,
                thread
            )) {

            elf64_loader_ownership_fail_stop();
        }

        if (!thread_destroy(thread)) {
            elf64_loader_ownership_fail_stop();
        }

        kfree(context);

        return ELF64_LOADER_STATUS_SCHEDULER_FAILED;
    }

    /*
     * Success: the scheduler owns the runnable TCB.
     * The thread entry routine will release context when it starts.
     */
    return ELF64_LOADER_STATUS_OK;
}

/*
 * Loader status descriptions.
 */
const char *elf64_loader_status_string(
    elf64_loader_status_t status
) {
    switch (status) {

        case ELF64_LOADER_STATUS_OK:
            return "success";

        case ELF64_LOADER_STATUS_INVALID_ARGUMENT:
            return "invalid argument";

        case ELF64_LOADER_STATUS_PARSE_FAILED:
            return "ELF parsing failed";

        case ELF64_LOADER_STATUS_OUT_OF_MEMORY:
            return "out of memory";

        case ELF64_LOADER_STATUS_PROCESS_STATE:
            return "process is not eligible for image loading";

        case ELF64_LOADER_STATUS_ADDRESS_CONFLICT:
            return "an ELF virtual page is already mapped";

        case ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR:
            return "address-space query failed";

        case ELF64_LOADER_STATUS_PREPARATION_FAILED:
            return "physical image-page preparation failed";

        case ELF64_LOADER_STATUS_OWNERSHIP_FAILED:
            return "process page ownership transfer failed";

        case ELF64_LOADER_STATUS_MAPPING_FAILED:
            return "one or more image pages could not be mapped";

        case ELF64_LOADER_STATUS_CLEANUP_FAILED:
            return "image cleanup could not complete";

        case ELF64_LOADER_STATUS_THREAD_CREATE_FAILED:
            return "initial user thread creation failed";

        case ELF64_LOADER_STATUS_SCHEDULER_FAILED:
            return "initial user thread could not be enqueued";

        default:
            return "unknown ELF loader status";
    }
}
