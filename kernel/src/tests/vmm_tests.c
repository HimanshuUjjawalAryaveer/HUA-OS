#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "drivers/console/console.h"
#include "mm/vmm.h"
#include "vmm_tests.h"

#include "mm/pmm.h"

#define VMM_TEST_PAGE_SIZE       4096ULL
#define VMM_TEST_HUGE_PAGE_SIZE  (2ULL * 1024ULL * 1024ULL)
#define VMM_TEST_HUGE_PAGE_COUNT 512ULL

/*
 * x86-64 page-table User/Supervisor permission bit.
 * Bit 2: 0 = supervisor, 1 = user-accessible.
 */
#define VMM_TEST_USER_BIT (1ULL << 2)

// Print a 64-bit value in hexadecimal without depending on main.c.
static void vmm_test_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buffer[19];

    buffer[0] = '0';
    buffer[1] = 'x';

    for (int i = 0; i < 16; i++) {
        unsigned int shift = (unsigned int)(60 - (i * 4));
        buffer[i + 2] = digits[(value >> shift) & 0xF];
    }

    buffer[18] = '\0';
    console_write(buffer);
}

static bool vmm_test_failure(const char *message) {
    console_set_color(0xFF5555, 0x101820);
    console_write("[ FAILED ] ");
    console_write(message);
    console_write("\n");
    console_set_color(0xFFFFFF, 0x101820);

    return false;
}

static void vmm_test_success(const char *message) {
    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] ");
    console_write(message);
    console_write("\n");
    console_set_color(0xFFFFFF, 0x101820);
}

#define VMM_TEST_FRAME_BATCH_SIZE 64ULL

/*
 * Allocate a physically contiguous run of PMM frames.
 *
 * PMM guarantees individual frame allocation, not
 * physical contiguity. Therefore, allocate a batch
 * and search for consecutive physical addresses.
 *
 * Only the selected frames remain allocated.
 */
static bool vmm_test_alloc_contiguous_frames(
    uint64_t page_count,
    uint64_t *physical_base_out
) {
    if (page_count == 0 ||
        page_count > VMM_TEST_FRAME_BATCH_SIZE ||
        physical_base_out == NULL) {
        return false;
    }

    *physical_base_out = 0;

    uint64_t frames[VMM_TEST_FRAME_BATCH_SIZE] = {0};
    uint64_t allocated_count = 0;

    /*
     * Allocate as many frames as possible, up to
     * the batch limit.
     */
    for (; allocated_count < VMM_TEST_FRAME_BATCH_SIZE;
         allocated_count++) {

        frames[allocated_count] = pmm_alloc_frame();

        if (frames[allocated_count] == 0) {
            break;
        }
    }

    /*
     * Search for physically consecutive frames.
     */
    uint64_t run_start = UINT64_MAX;

    for (uint64_t i = 0;
         i + page_count <= allocated_count;
         i++) {

        bool contiguous = true;

        for (uint64_t j = 1; j < page_count; j++) {

            if (frames[i + j] !=
                frames[i] + j * VMM_TEST_PAGE_SIZE) {

                contiguous = false;
                break;
            }
        }

        if (contiguous) {
            run_start = i;
            break;
        }
    }

    /*
     * If no contiguous run exists, release every
     * frame allocated by this helper.
     */
    if (run_start == UINT64_MAX) {

        for (uint64_t i = 0; i < allocated_count; i++) {
            pmm_free_frame(frames[i]);
        }

        return false;
    }

    /*
     * Release every frame outside the selected run.
     */
    for (uint64_t i = 0; i < allocated_count; i++) {

        if (i >= run_start &&
            i < run_start + page_count) {
            continue;
        }

        if (!pmm_free_frame(frames[i])) {

            /*
             * Release the selected run as well if
             * cleanup of an unused frame fails.
             */
            for (uint64_t j = 0; j < page_count; j++) {
                pmm_free_frame(frames[run_start + j]);
            }

            return false;
        }
    }

    *physical_base_out = frames[run_start];

    return true;
}

/*
 * Safely remove any mappings in a physical range and
 * release the caller-owned physical frames.
 *
 * A physical frame is released only after every
 * existing mapping has been successfully removed
 * and its physical address has been verified.
 */
static bool vmm_test_release_range(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t page_count
) {
    if (page_count == 0) {
        return false;
    }

    bool cleanup_valid = true;

    /*
     * First remove all mappings that exist.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t current_virtual =
            virtual_address + i * VMM_TEST_PAGE_SIZE;

        uint64_t expected_physical =
            physical_address + i * VMM_TEST_PAGE_SIZE;

        vmm_mapping_info_t mapping;

        vmm_status_t query_status =
            vmm_query(current_virtual, &mapping);

        if (query_status == VMM_STATUS_NOT_MAPPED) {
            continue;
        }

        if (query_status != VMM_STATUS_OK) {
            cleanup_valid = false;
            continue;
        }

        uint64_t removed_physical = 0;

        vmm_status_t unmap_status =
            vmm_unmap_page(
                current_virtual,
                &removed_physical
            );

        if (unmap_status != VMM_STATUS_OK ||
            removed_physical != expected_physical) {

            cleanup_valid = false;
        }
    }

    /*
     * Never release data frames if mapping cleanup
     * was unsuccessful.
     */
    if (!cleanup_valid) {
        return false;
    }

    /*
     * All mappings are now removed.
     * Release the physical frames through PMM.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t current_physical =
            physical_address + i * VMM_TEST_PAGE_SIZE;

        if (!pmm_free_frame(current_physical)) {
            return false;
        }
    }

    return true;
}


/*
 * Address-space isolation, mapping ownership, reference
 * counting, and recursive destruction tests.
 *
 * These tests do not activate another CR3. They inspect
 * independently owned page-table roots through the VMM API.
 */
static bool vmm_test_address_spaces(void) {

    static const uint64_t test_virtual_addresses[] = {
        0x0000000000400000ULL,
        0x0000000000800000ULL,
        0x0000000040000000ULL,
        0x0000000080000000ULL,
        0x0000008000000000ULL
    };

    #define ADDRESS_SPACE_TEST_MAPPING_COUNT 5

    const uint64_t mapping_flags =
        VMM_MAP_USER | VMM_MAP_WRITABLE;

    vmm_address_space_t *space_a = NULL;
    vmm_address_space_t *space_b = NULL;

    uint64_t root_a = 0;
    uint64_t root_b = 0;

    uint64_t frames_a[ADDRESS_SPACE_TEST_MAPPING_COUNT] = {0};
    uint64_t frame_b = 0;

    bool frame_a_allocated[ADDRESS_SPACE_TEST_MAPPING_COUNT] = {false};
    bool frame_b_allocated = false;

    uint64_t references_a = 0;
    bool a_destroyed = false;
    bool b_destroyed = false;
    bool b_mapping_removed = false;

    /*
    * State for reversible teardown-corruption tests.
    *
    * These flags also allow the failure path to restore
    * injected corruption before attempting normal cleanup.
    */
    bool duplicate_reference_injected = false;

    uint64_t duplicate_alias_index = UINT64_MAX;
    uint64_t duplicate_entry = 0;

    bool orphan_record_injected = false;
    uint64_t orphan_frame = 0;

    vmm_mapping_info_t mapping;

    space_a = vmm_address_space_create();

    if (space_a == NULL) {
        return vmm_test_failure(
            "Second address-space creation"
        );
    }

    references_a = 1;

    if (vmm_address_space_get(NULL) ||
        vmm_address_space_put(NULL) ||
        vmm_address_space_map_page(
            NULL,
            test_virtual_addresses[0],
            0,
            mapping_flags
        ) != VMM_STATUS_INVALID_ARGUMENT ||
        vmm_address_space_unmap_page(
            NULL,
            test_virtual_addresses[0],
            &root_a
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_address_space_map_page(
            vmm_kernel_address_space(),
            test_virtual_addresses[0],
            0,
            mapping_flags
        ) != VMM_STATUS_INVALID_ARGUMENT ||
        vmm_address_space_unmap_page(
            vmm_kernel_address_space(),
            test_virtual_addresses[0],
            &root_a
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_address_space_put(
            vmm_kernel_address_space()
        )) {

        goto failure;
    }

    vmm_test_success(
        "Address-space invalid argument and kernel protection"
    );


    /*
     * TEST 2:
     * Create two independent address spaces.
     */
    space_b = vmm_address_space_create();

    if (space_b == NULL) {
        goto failure;
    }

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
        (root_a & (VMM_TEST_PAGE_SIZE - 1)) != 0 ||
        (root_b & (VMM_TEST_PAGE_SIZE - 1)) != 0) {

        goto failure;
    }

    vmm_test_success(
        "Independent address-space roots"
    );


    /*
     * TEST 3:
     * Allocate caller-owned physical data frames.
     */
    for (uint64_t i = 0;
         i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
         i++) {

        frames_a[i] = pmm_alloc_frame();

        if (frames_a[i] == 0 ||
            pmm_frame_to_virt(frames_a[i]) == NULL) {

            goto failure;
        }

        frame_a_allocated[i] = true;
    }

    frame_b = pmm_alloc_frame();

    if (frame_b == 0 ||
        pmm_frame_to_virt(frame_b) == NULL) {

        goto failure;
    }

    frame_b_allocated = true;

    for (uint64_t i = 0;
         i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
         i++) {

        if (frames_a[i] == frame_b) {
            goto failure;
        }

        for (uint64_t j = i + 1;
             j < ADDRESS_SPACE_TEST_MAPPING_COUNT;
             j++) {

            if (frames_a[i] == frames_a[j]) {
                goto failure;
            }
        }
    }

    vmm_test_success(
        "Address-space test frame allocation"
    );


    /*
     * TEST 4:
     * Reject invalid mapping permissions and addresses.
     */

    /*
    * TEST 4A:
    * Create a supervisor-only leaf first.
    * Then create a user-accessible leaf beneath the same
    * intermediate page-table hierarchy.
    */
    const uint64_t supervisor_first_va =
        test_virtual_addresses[0];

    const uint64_t user_second_va =
        supervisor_first_va + VMM_TEST_PAGE_SIZE;

    if (vmm_address_space_map_page(
            space_a,
            supervisor_first_va,
            frames_a[0],
            VMM_MAP_NONE
        ) != VMM_STATUS_OK) {

        goto failure;
    }

    if (vmm_address_space_map_page(
            space_a,
            user_second_va,
            frames_a[1],
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        goto failure;
    }

    /* Verify the supervisor-only leaf. */
    if (vmm_address_space_query(
            space_a,
            supervisor_first_va,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frames_a[0] ||
        mapping.page_size != VMM_TEST_PAGE_SIZE ||
        mapping.huge_page ||
        mapping.user_accessible ||
        mapping.writable ||
        mapping.executable) {

        goto failure;
    }

    /* Verify the user-accessible leaf. */
    if (vmm_address_space_query(
            space_a,
            user_second_va,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frames_a[1] ||
        mapping.page_size != VMM_TEST_PAGE_SIZE ||
        mapping.huge_page ||
        !mapping.user_accessible ||
        !mapping.writable ||
        mapping.executable) {

        goto failure;
    }

    /*
    * Remove both mappings and verify the returned frames.
    * The frames remain caller-owned and are reused below.
    */
    uint64_t removed_supervisor_first = 0;
    uint64_t removed_user_second = 0;

    if (vmm_address_space_unmap_page(
            space_a,
            supervisor_first_va,
            &removed_supervisor_first
        ) != VMM_STATUS_OK ||
        removed_supervisor_first != frames_a[0]) {

        goto failure;
    }

    if (vmm_address_space_unmap_page(
            space_a,
            user_second_va,
            &removed_user_second
        ) != VMM_STATUS_OK ||
        removed_user_second != frames_a[1]) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_a,
            supervisor_first_va,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_address_space_query(
            space_a,
            user_second_va,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto failure;
    }

    vmm_test_success(
        "Supervisor-first and user-second mixed mappings"
    );

    /*
    * TEST 4B:
    * Reverse the mapping order in a different PML4 branch.
    *
    * This verifies that an existing user-permissive hierarchy
    * can safely contain a supervisor-only leaf.
    */
    const uint64_t user_first_va =
        test_virtual_addresses[4];

    const uint64_t supervisor_second_va =
        user_first_va + VMM_TEST_PAGE_SIZE;

    if (vmm_address_space_map_page(
            space_a,
            user_first_va,
            frames_a[2],
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        goto failure;
    }

    if (vmm_address_space_map_page(
            space_a,
            supervisor_second_va,
            frames_a[3],
            VMM_MAP_NONE
        ) != VMM_STATUS_OK) {

        goto failure;
    }

    /* Verify the user-accessible leaf. */
    if (vmm_address_space_query(
            space_a,
            user_first_va,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frames_a[2] ||
        mapping.page_size != VMM_TEST_PAGE_SIZE ||
        mapping.huge_page ||
        !mapping.user_accessible ||
        !mapping.writable ||
        mapping.executable) {

        goto failure;
    }

    /* Verify the supervisor-only leaf. */
    if (vmm_address_space_query(
            space_a,
            supervisor_second_va,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frames_a[3] ||
        mapping.page_size != VMM_TEST_PAGE_SIZE ||
        mapping.huge_page ||
        mapping.user_accessible ||
        mapping.writable ||
        mapping.executable) {

        goto failure;
    }

    uint64_t removed_user_first = 0;
    uint64_t removed_supervisor_second = 0;

    if (vmm_address_space_unmap_page(
            space_a,
            user_first_va,
            &removed_user_first
        ) != VMM_STATUS_OK ||
        removed_user_first != frames_a[2]) {

        goto failure;
    }

    if (vmm_address_space_unmap_page(
            space_a,
            supervisor_second_va,
            &removed_supervisor_second
        ) != VMM_STATUS_OK ||
        removed_supervisor_second != frames_a[3]) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_a,
            user_first_va,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_address_space_query(
            space_a,
            supervisor_second_va,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto failure;
    }

    vmm_test_success(
        "User-first and supervisor-second mixed mappings"
    );

    if (vmm_address_space_map_page(
            space_a,
            test_virtual_addresses[0],
            frames_a[0],
            VMM_MAP_USER | VMM_MAP_GLOBAL
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_address_space_map_page(
            space_a,
            test_virtual_addresses[0],
            frames_a[0],
            VMM_MAP_USER |
            VMM_MAP_WRITABLE |
            VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_address_space_map_page(
            space_a,
            test_virtual_addresses[0] + 1,
            frames_a[0],
            mapping_flags
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    /*
    * A non-canonical virtual address must be rejected
    * explicitly as NON_CANONICAL.
    */
    if (vmm_address_space_map_page(
            space_a,
            0x0000800000000000ULL,
            frames_a[0],
            mapping_flags
        ) != VMM_STATUS_NON_CANONICAL) {

        goto failure;
    }

    /*
    * A canonical upper-half address must not be mapped
    * into an independent user address space.
    */
    if (vmm_address_space_map_page(
            space_a,
            0xFFFF800000000000ULL,
            frames_a[0],
            mapping_flags
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_address_space_map_page(
            space_a,
            test_virtual_addresses[0],
            frames_a[0],
            mapping_flags | (1ULL << 12)
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_address_space_map_page(
            space_a,
            test_virtual_addresses[0],
            UINT64_MAX & ~(VMM_TEST_PAGE_SIZE - 1),
            mapping_flags
        ) != VMM_STATUS_INVALID_PHYSICAL_ADDRESS) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_a,
            test_virtual_addresses[0],
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto failure;
    }

    vmm_test_success(
        "Address-space mapping argument and permission validation"
    );


    /*
     * TEST 5:
     * Map five pages into address space A.
     *
     * These addresses exercise:
     * - Multiple page-table branches.
     * - Different PD indexes.
     * - Different PDPT indexes.
     * - A different PML4 index.
     */
    for (uint64_t i = 0;
         i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
         i++) {

        if (vmm_address_space_map_page(
                space_a,
                test_virtual_addresses[i],
                frames_a[i],
                mapping_flags
            ) != VMM_STATUS_OK) {

            goto failure;
        }
    }

    vmm_test_success(
        "Address-space mappings across independent paging branches"
    );


    /*
     * TEST 6:
     * Reject a duplicate mapping without changing
     * the original translation.
     */
    if (vmm_address_space_map_page(
            space_a,
            test_virtual_addresses[0],
            frames_a[1],
            mapping_flags
        ) != VMM_STATUS_ALREADY_MAPPED) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_a,
            test_virtual_addresses[0],
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frames_a[0]) {

        goto failure;
    }

    vmm_test_success(
        "Duplicate mapping rejection preserves original translation"
    );


    /*
     * TEST 7:
     * Verify the effective mapping permissions.
     */
    for (uint64_t i = 0;
         i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
         i++) {

        if (vmm_address_space_query(
                space_a,
                test_virtual_addresses[i],
                &mapping
            ) != VMM_STATUS_OK) {

            goto failure;
        }

        if (mapping.physical_address != frames_a[i] ||
            mapping.page_size != VMM_TEST_PAGE_SIZE ||
            mapping.huge_page ||
            !mapping.user_accessible ||
            !mapping.writable ||
            mapping.executable) {

            goto failure;
        }
    }

    vmm_test_success(
        "Address-space physical translations and effective permissions"
    );


    /*
     * TEST 8:
     * Map the same virtual address in B to a different
     * physical frame.
     */
    if (vmm_address_space_map_page(
            space_b,
            test_virtual_addresses[0],
            frame_b,
            mapping_flags
        ) != VMM_STATUS_OK) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_b,
            test_virtual_addresses[0],
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frame_b) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_a,
            test_virtual_addresses[0],
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frames_a[0]) {

        goto failure;
    }

    if (vmm_query(
            test_virtual_addresses[0],
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto failure;
    }

    vmm_test_success(
        "Same virtual address isolation between address spaces"
    );


    /*
     * TEST 9:
     * Reference counting.
     *
     * An extra reference must prevent destruction
     * when the original reference is released.
     */
    if (!vmm_address_space_get(space_a)) {
        goto failure;
    }

    references_a++;

    if (!vmm_address_space_put(space_a)) {
        goto failure;
    }

    references_a--;

    if (vmm_address_space_query(
            space_a,
            test_virtual_addresses[0],
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != frames_a[0]) {

        goto failure;
    }

    vmm_test_success(
        "Address-space reference counting"
    );


    /*
     * TEST 10:
     * Explicit unmapping returns the caller-owned frame.
     */
    uint64_t returned_frame = 0;

    if (vmm_address_space_unmap_page(
            space_b,
            test_virtual_addresses[0],
            &returned_frame
        ) != VMM_STATUS_OK ||
        returned_frame != frame_b) {

        goto failure;
    }

    b_mapping_removed = true;

    if (vmm_address_space_query(
            space_b,
            test_virtual_addresses[0],
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto failure;
    }

    if (pmm_frame_to_virt(frame_b) == NULL) {
        goto failure;
    }

    if (!pmm_free_frame(frame_b)) {
        goto failure;
    }

    frame_b_allocated = false;

    vmm_test_success(
        "Address-space unmapping preserves caller frame ownership"
    );


    /*
    * TEST 11:
    * Duplicate page-table reference detection.
    *
    * A single owned page-table frame is deliberately made
    * reachable through two PML4 entries.
    *
    * Final destruction must reject this topology before
    * reclaiming any intermediate table.
    */
    if (!vmm_test_inject_duplicate_root_entry(
            space_a,
            &duplicate_alias_index,
            &duplicate_entry
        )) {

        goto failure;
    }

    duplicate_reference_injected = true;

    /*
    * Destruction must fail because the same owned table
    * is encountered more than once during validation.
    */
    if (vmm_address_space_put(space_a)) {
        goto failure;
    }

    /*
    * Verify that every mapping remains present and unchanged.
    * A rejected teardown must not partially destroy the tree.
    */
    for (uint64_t i = 0;
        i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
        i++) {

        if (vmm_address_space_query(
                space_a,
                test_virtual_addresses[i],
                &mapping
            ) != VMM_STATUS_OK ||
            mapping.physical_address != frames_a[i]) {

            goto failure;
        }
    }

    /*
    * Restore the exact root entry injected by the hook.
    */
    if (!vmm_test_restore_duplicate_root_entry(
            space_a,
            duplicate_alias_index,
            duplicate_entry
        )) {

        goto failure;
    }

    duplicate_reference_injected = false;

    duplicate_alias_index = UINT64_MAX;
    duplicate_entry = 0;

    vmm_test_success(
        "Address-space teardown rejects duplicate page-table references"
    );


    /*
    * TEST 12:
    * Orphan ownership-record detection.
    *
    * Register an allocated table frame for A without linking
    * that frame into A's actual page-table hierarchy.
    *
    * Final destruction must reject the unvisited ownership
    * record before reclaiming any table.
    */
    if (!vmm_test_inject_orphan_owned_table(
            space_a,
            &orphan_frame
        )) {

        goto failure;
    }

    orphan_record_injected = true;

    if (pmm_frame_to_virt(orphan_frame) == NULL) {
        goto failure;
    }

    /*
    * The orphan scan must reject this address space.
    */
    if (vmm_address_space_put(space_a)) {
        goto failure;
    }

    /*
    * Again verify that rejection did not partially destroy
    * the real mappings.
    */
    for (uint64_t i = 0;
        i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
        i++) {

        if (vmm_address_space_query(
                space_a,
                test_virtual_addresses[i],
                &mapping
            ) != VMM_STATUS_OK ||
            mapping.physical_address != frames_a[i]) {

            goto failure;
        }
    }

    /*
    * Remove the injected record and return its frame to PMM.
    */
    if (!vmm_test_restore_orphan_owned_table(
            space_a,
            orphan_frame
        )) {

        goto failure;
    }

    orphan_record_injected = false;
    orphan_frame = 0;

    vmm_test_success(
        "Address-space teardown rejects orphan ownership records"
    );


    /*
    * TEST 13:
    * Final destruction of A while five mappings
    * remain present.
    *
    * After both corruption scenarios are restored,
    * normal recursive teardown must succeed.
    */
    if (!vmm_address_space_put(space_a)) {
        goto failure;
    }

    references_a--;
    a_destroyed = true;
    space_a = NULL;

    /*
     * Verify that the caller-owned data frames survived
     * address-space destruction.
     */
    for (uint64_t i = 0;
         i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
         i++) {

        if (pmm_frame_to_virt(frames_a[i]) == NULL) {
            goto failure;
        }
    }

    vmm_test_success(
        "Recursive address-space teardown preserves data frames"
    );


    /*
     * TEST 12:
     * A destroyed address space must no longer be
     * accepted by the registry.
     *
     * No allocation is performed between destruction
     * and these stale-pointer checks.
     */
    if (vmm_address_space_get(space_a) ||
        vmm_address_space_get_root_physical(
            space_a,
            &root_a
        )) {

        goto failure;
    }

    vmm_test_success(
        "Destroyed address-space pointer rejection"
    );


    /*
     * TEST 13:
     * Destroy B independently.
     */
    if (!vmm_address_space_put(space_b)) {
        goto failure;
    }

    b_destroyed = true;
    space_b = NULL;

    vmm_test_success(
        "Independent address-space destruction"
    );


    /*
     * TEST 14:
     * Release caller-owned data frames only after
     * their address spaces have been destroyed.
     */
    for (uint64_t i = 0;
         i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
         i++) {

        if (!pmm_free_frame(frames_a[i])) {
            goto failure;
        }

        frame_a_allocated[i] = false;
    }

    vmm_test_success(
        "Address-space teardown PMM ownership verification"
    );

    vmm_test_success(
        "VMM address-space comprehensive tests"
    );

    return true;

failure:

    /*
    * Restore any outstanding teardown-test corruption first.
    *
    * Never attempt ordinary address-space destruction while
    * our deliberate corruption is still installed.
    */
    if (space_a != NULL && duplicate_reference_injected) {

        if (vmm_test_restore_duplicate_root_entry(
                space_a,
                duplicate_alias_index,
                duplicate_entry
            )) {

            duplicate_reference_injected = false;
            duplicate_alias_index = UINT64_MAX;
            duplicate_entry = 0;
        }
    }


    if (space_a != NULL && orphan_record_injected) {

        if (vmm_test_restore_orphan_owned_table(
                space_a,
                orphan_frame
            )) {

            orphan_record_injected = false;
            orphan_frame = 0;
        }
    }

    /*
     * Best-effort cleanup.
     *
     * Never free data frames while a surviving address
     * space might still reference them.
     */
    if (space_a != NULL) {

        while (references_a > 0) {

            if (!vmm_address_space_put(space_a)) {
                break;
            }

            references_a--;
        }

        if (references_a == 0) {
            a_destroyed = true;
            space_a = NULL;
        }
    }

    if (space_b != NULL) {

        if (vmm_address_space_put(space_b)) {
            b_destroyed = true;
            space_b = NULL;
        }
    }

    if (a_destroyed) {

        for (uint64_t i = 0;
             i < ADDRESS_SPACE_TEST_MAPPING_COUNT;
             i++) {

            if (frame_a_allocated[i]) {
                pmm_free_frame(frames_a[i]);
            }
        }
    }

    if (b_destroyed || b_mapping_removed) {

        if (frame_b_allocated) {
            pmm_free_frame(frame_b);
        }
    }

    #undef ADDRESS_SPACE_TEST_MAPPING_COUNT

    return vmm_test_failure(
        "VMM address-space comprehensive tests"
    );
}

/*
 * Test synchronization of newly created kernel upper-half
 * mappings into already existing process address spaces.
 *
 * Also verify that:
 * - Kernel mappings remain supervisor-only.
 * - Kernel-root APIs reject VMM_MAP_USER.
 * - Removing the final mapping removes the mirrored
 *   PML4 entry from every process root.
 */
static bool vmm_test_kernel_mapping_mirroring(void) {

    vmm_address_space_t *space_a = NULL;
    vmm_address_space_t *space_b = NULL;

    uint64_t kernel_root_physical = 0;
    uint64_t root_a_physical = 0;
    uint64_t root_b_physical = 0;

    uint64_t kernel_entry = 0;
    uint64_t entry_a = 0;
    uint64_t entry_b = 0;

    uint64_t pml4_index = UINT64_MAX;
    uint64_t test_virtual_address = 0;

    uint64_t data_frame = 0;
    uint64_t returned_frame = 0;

    bool data_frame_allocated = false;
    bool mapping_present = false;

    uint64_t range_returned[1] = {0};

    vmm_mapping_info_t mapping;

    /*
     * Create both process address spaces before creating
     * the test kernel mapping.
     */
    space_a = vmm_address_space_create();

    if (space_a == NULL) {
        goto failure;
    }

    space_b = vmm_address_space_create();

    if (space_b == NULL) {
        goto failure;
    }

    /*
     * Obtain all three PML4 roots.
     */
    if (!vmm_address_space_get_root_physical(
            vmm_kernel_address_space(),
            &kernel_root_physical
        ) ||
        !vmm_address_space_get_root_physical(
            space_a,
            &root_a_physical
        ) ||
        !vmm_address_space_get_root_physical(
            space_b,
            &root_b_physical
        )) {

        goto failure;
    }

    if (kernel_root_physical == 0 ||
        root_a_physical == 0 ||
        root_b_physical == 0 ||
        root_a_physical == root_b_physical ||
        (kernel_root_physical & (VMM_TEST_PAGE_SIZE - 1)) != 0 ||
        (root_a_physical & (VMM_TEST_PAGE_SIZE - 1)) != 0 ||
        (root_b_physical & (VMM_TEST_PAGE_SIZE - 1)) != 0) {

        goto failure;
    }

    /*
     * Find a completely unused upper-half PML4 slot.
     *
     * The slot must be empty in the kernel root and both
     * process roots before the test begins.
     */

    for (uint64_t i = 256; i < 512; i++) {

        if (!vmm_test_read_pml4_entry(
                vmm_kernel_address_space(),
                i,
                &kernel_entry
            ) ||
            !vmm_test_read_pml4_entry(
                space_a,
                i,
                &entry_a
            ) ||
            !vmm_test_read_pml4_entry(
                space_b,
                i,
                &entry_b
            )) {

            goto failure;
        }

        if (kernel_entry == 0 &&
            entry_a == 0 &&
            entry_b == 0) {

            pml4_index = i;
            break;
        }
    }

    if (pml4_index == UINT64_MAX) {
        goto failure;
    }

    /*
     * Construct a canonical upper-half address within
     * the selected PML4 slot.
     */
    test_virtual_address =
        0xFFFF000000000000ULL |
        (pml4_index << 39);

    if (test_virtual_address & (VMM_TEST_PAGE_SIZE - 1)) {
        goto failure;
    }

    /*
     * Allocate one caller-owned physical data frame.
     */
    data_frame = pmm_alloc_frame();

    if (data_frame == 0 ||
        pmm_frame_to_virt(data_frame) == NULL) {

        goto failure;
    }

    data_frame_allocated = true;


    /*
     * TEST A:
     * Create a new upper-half kernel mapping.
     */
    if (vmm_map_page(
            test_virtual_address,
            data_frame,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        goto failure;
    }

    mapping_present = true;

    /*
     * The new kernel PML4 entry must be present and
     * supervisor-only.
     */
    if (!vmm_test_read_pml4_entry(
            vmm_kernel_address_space(),
            pml4_index,
            &kernel_entry
        )) {

        goto failure;
    }

    if (!(kernel_entry & 1ULL) ||
        (kernel_entry & VMM_TEST_USER_BIT)) {

        goto failure;
    }

    /*
     * Both existing process roots must now contain
     * the corresponding supervisor-only entry.
     */
    uint64_t expected_process_entry =
        kernel_entry & ~VMM_TEST_USER_BIT;

    if (!vmm_test_read_pml4_entry(
            space_a,
            pml4_index,
            &entry_a
        ) ||
        !vmm_test_read_pml4_entry(
            space_b,
            pml4_index,
            &entry_b
        ) ||
        entry_a != expected_process_entry ||
        entry_b != expected_process_entry) {

        goto failure;
    }

    /*
     * Query the mapping through all three address spaces.
     */
    if (vmm_query(
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != data_frame ||
        mapping.user_accessible) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_a,
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != data_frame ||
        mapping.user_accessible) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_b,
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != data_frame ||
        mapping.user_accessible) {

        goto failure;
    }

    vmm_test_success(
        "Kernel upper-half mapping synchronization and supervisor isolation"
    );


    /*
     * TEST B:
     * Kernel-root APIs must reject user-accessible mappings
     * and permission upgrades.
     */
    if (vmm_map_page(
            test_virtual_address + VMM_TEST_PAGE_SIZE,
            data_frame,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_map_range(
            test_virtual_address + VMM_TEST_PAGE_SIZE,
            data_frame,
            1,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    if (vmm_protect_page(
            test_virtual_address,
            VMM_MAP_USER
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        goto failure;
    }

    /*
     * The original valid mapping must remain supervisor-only.
     */
    if (vmm_query(
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != data_frame ||
        mapping.user_accessible) {

        goto failure;
    }

    vmm_test_success(
        "Kernel mapping and protection APIs reject user permissions"
    );


    /*
     * TEST C:
     * Remove the sole kernel mapping.
     *
     * The kernel unmapper should reclaim the newly created
     * intermediate tables and synchronize the now-empty
     * root entry into both process address spaces.
     */
    if (vmm_unmap_page(
            test_virtual_address,
            &returned_frame
        ) != VMM_STATUS_OK ||
        returned_frame != data_frame) {

        goto failure;
    }

    mapping_present = false;

    if (!vmm_test_read_pml4_entry(
            vmm_kernel_address_space(),
            pml4_index,
            &kernel_entry
        ) ||
        !vmm_test_read_pml4_entry(
            space_a,
            pml4_index,
            &entry_a
        ) ||
        !vmm_test_read_pml4_entry(
            space_b,
            pml4_index,
            &entry_b
        ) ||
        kernel_entry != 0 ||
        entry_a != 0 ||
        entry_b != 0) {

        goto failure;
    }

    if (vmm_address_space_query(
            space_a,
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_address_space_query(
            space_b,
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_query(
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto failure;
    }

    vmm_test_success(
        "Kernel upper-half mapping removal synchronization"
    );


    /*
     * TEST D:
     * Exercise the range APIs through the same synchronized
     * internal wrappers.
     *
     * A one-page range is intentional: it exercises the
     * range transaction without requiring contiguous
     * physical-frame allocation.
     */
    if (vmm_map_range(
            test_virtual_address,
            data_frame,
            1,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        goto failure;
    }

    mapping_present = true;

    if (vmm_address_space_query(
            space_a,
            test_virtual_address,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != data_frame) {

        goto failure;
    }

    if (vmm_unmap_range(
            test_virtual_address,
            1,
            range_returned
        ) != VMM_STATUS_OK ||
        range_returned[0] != data_frame) {

        goto failure;
    }

    mapping_present = false;

    if (!vmm_test_read_pml4_entry(
            vmm_kernel_address_space(),
            pml4_index,
            &kernel_entry
        ) ||
        !vmm_test_read_pml4_entry(
            space_a,
            pml4_index,
            &entry_a
        ) ||
        !vmm_test_read_pml4_entry(
            space_b,
            pml4_index,
            &entry_b
        ) ||
        kernel_entry != 0 ||
        entry_a != 0 ||
        entry_b != 0) {

        goto failure;
    }

    vmm_test_success(
        "Kernel mapping range synchronization"
    );


    /*
     * The physical data frame remains caller-owned.
     */
    if (!pmm_free_frame(data_frame)) {
        goto failure;
    }

    data_frame_allocated = false;
    data_frame = 0;


    /*
     * Both process roots can now be destroyed independently.
     */
    if (!vmm_address_space_put(space_a)) {
        goto failure;
    }

    space_a = NULL;

    if (!vmm_address_space_put(space_b)) {
        goto failure;
    }

    space_b = NULL;

    vmm_test_success(
        "Kernel mapping synchronization ownership and cleanup"
    );

    return true;


failure:

    /*
     * Never free the physical data frame while a kernel
     * mapping could still reference it.
     */
    if (mapping_present) {

        uint64_t cleanup_frame = 0;

        if (vmm_unmap_page(
                test_virtual_address,
                &cleanup_frame
            ) == VMM_STATUS_OK &&
            cleanup_frame == data_frame) {

            mapping_present = false;
        }
    }

    if (!mapping_present &&
        data_frame_allocated) {

        if (pmm_free_frame(data_frame)) {
            data_frame_allocated = false;
        }
    }

    /*
     * Best-effort process address-space cleanup.
     */
    if (space_b != NULL) {
        vmm_address_space_put(space_b);
    }

    if (space_a != NULL) {
        vmm_address_space_put(space_a);
    }

    return vmm_test_failure(
        "Kernel upper-half mapping synchronization"
    );
}

/*
 * Verify address-space activation, CR3 switching,
 * active CPU bookkeeping and active-space destruction
 * rejection.
 */
static bool vmm_test_address_space_activation(void) {

    vmm_address_space_t *space = NULL;
    vmm_address_space_t *active_space = NULL;

    uint64_t kernel_root = 0;
    uint64_t process_root = 0;
    uint64_t active_root = 0;

    uint64_t kernel_mask = 0;
    uint64_t process_mask = 0;

    bool process_space_active = false;

    /*
     * The initial hardware CR3 must correspond to
     * the permanent kernel address space.
     */
    if (!vmm_address_space_get_root_physical(
            vmm_kernel_address_space(),
            &kernel_root
        ) ||
        !vmm_test_get_active_address_space(
            0,
            &active_space,
            &active_root
        ) ||
        active_space != vmm_kernel_address_space() ||
        active_root != kernel_root) {

        goto failure;
    }

    /*
     * CPU0's kernel address space must have exactly
     * its CPU0 active bit set.
     */
    if (!vmm_test_get_address_space_active_cpu_mask(
            vmm_kernel_address_space(),
            &kernel_mask
        ) ||
        kernel_mask != 1ULL) {

        goto failure;
    }

    /*
     * Create one independently owned address space.
     */
    space = vmm_address_space_create();

    if (space == NULL) {
        goto failure;
    }

    if (!vmm_address_space_get_root_physical(
            space,
            &process_root
        ) ||
        process_root == 0 ||
        process_root == kernel_root ||
        (process_root & (VMM_TEST_PAGE_SIZE - 1)) != 0) {

        goto failure;
    }

    /*
     * A new address space must initially be inactive.
     */
    if (!vmm_test_get_address_space_active_cpu_mask(
            space,
            &process_mask
        ) ||
        process_mask != 0) {

        goto failure;
    }

    /*
     * Invalid CPU identifiers and NULL spaces must fail.
     */
     if (vmm_address_space_activate(1, space) ||
        vmm_address_space_activate(64, space) ||
        vmm_address_space_activate(0, NULL)) {

        goto failure;
    }

    /*
     * Failed activation must not disturb the current CR3.
     */
    if (!vmm_test_get_active_address_space(
            0,
            &active_space,
            &active_root
        ) ||
        active_space != vmm_kernel_address_space() ||
        active_root != kernel_root) {

        goto failure;
    }

    /*
     * Activate the process address space.
     */
    if (!vmm_address_space_activate(0, space)) {
        goto failure;
    }

    process_space_active = true;

    /*
     * Verify both software bookkeeping and actual CR3.
     */
    if (!vmm_test_get_active_address_space(
            0,
            &active_space,
            &active_root
        ) ||
        active_space != space ||
        active_root != process_root) {

        goto failure;
    }

    if (!vmm_test_get_address_space_active_cpu_mask(
            space,
            &process_mask
        ) ||
        process_mask != 1ULL) {

        goto failure;
    }

    if (!vmm_test_get_address_space_active_cpu_mask(
            vmm_kernel_address_space(),
            &kernel_mask
        ) ||
        kernel_mask != 0) {

        goto failure;
    }

    vmm_test_success(
        "CR3 activation and active CPU tracking"
    );

    /*
     * Repeating activation of the current address space
     * must be harmless and must not corrupt the masks.
     */
    if (!vmm_address_space_activate(0, space)) {
        goto failure;
    }

    if (!vmm_test_get_active_address_space(
            0,
            &active_space,
            &active_root
        ) ||
        active_space != space ||
        active_root != process_root) {

        goto failure;
    }

    vmm_test_success(
        "Idempotent address-space activation"
    );

    /*
     * Final-reference destruction must fail while
     * the space is active.
     *
     * The failed attempt must preserve the reference.
     */
    if (vmm_address_space_put(space)) {
        goto failure;
    }

    if (!vmm_address_space_get_root_physical(
            space,
            &active_root
        ) ||
        active_root != process_root) {

        goto failure;
    }

    /*
     * A temporary extra reference may be acquired and
     * released while the address space is active.
     */
    if (!vmm_address_space_get(space) ||
        !vmm_address_space_put(space)) {

        goto failure;
    }

    if (!vmm_test_get_address_space_active_cpu_mask(
            space,
            &process_mask
        ) ||
        process_mask != 1ULL) {

        goto failure;
    }

    vmm_test_success(
        "Active address space cannot be destroyed"
    );

    /*
     * Switch back to the permanent kernel address space.
     */
    if (!vmm_address_space_activate(
            0,
            vmm_kernel_address_space()
        )) {

        goto failure;
    }

    process_space_active = false;

    /*
     * Confirm CR3 and both masks changed correctly.
     */
    if (!vmm_test_get_active_address_space(
            0,
            &active_space,
            &active_root
        ) ||
        active_space != vmm_kernel_address_space() ||
        active_root != kernel_root) {

        goto failure;
    }

    if (!vmm_test_get_address_space_active_cpu_mask(
            space,
            &process_mask
        ) ||
        process_mask != 0) {

        goto failure;
    }

    if (!vmm_test_get_address_space_active_cpu_mask(
            vmm_kernel_address_space(),
            &kernel_mask
        ) ||
        kernel_mask != 1ULL) {

        goto failure;
    }

    vmm_test_success(
        "CR3 restoration to kernel address space"
    );

    /*
    * Preserve the pointer value solely for a stale-pointer test.
    * Do not dereference it after destruction.
    */
    vmm_address_space_t *stale_space = space;

    if (!vmm_address_space_put(space)) {
        goto failure;
    }

    space = NULL;

    /*
    * A destroyed address space must not be activated.
    */
    if (vmm_address_space_activate(0, stale_space)) {
        goto failure;
    }

    vmm_test_success(
        "Inactive address-space destruction and stale-pointer rejection"
    );

    return true;


failure:

    /*
     * Never attempt to destroy the test space while it
     * might still be active.
     */
    if (process_space_active) {

        if (vmm_address_space_activate(
                0,
                vmm_kernel_address_space()
            )) {

            process_space_active = false;
        }
    }

    if (space != NULL && !process_space_active) {

        if (vmm_address_space_put(space)) {
            space = NULL;
        }
    }

    return vmm_test_failure(
        "Address-space activation and CR3 tests"
    );
}

bool vmm_address_space_comprehensive_tests(void) {

    console_write(
        "[ TEST ] Comprehensive independent address spaces...\n"
    );

    if (!vmm_test_address_spaces()) {
        return false;
    }

    console_write(
        "[ TEST ] Kernel mapping synchronization...\n"
    );

    if (!vmm_test_kernel_mapping_mirroring()) {
        return false;
    }

    console_write(
        "[ TEST ] Address-space activation...\n"
    );

    return vmm_test_address_space_activation();
}

bool vmm_run_tests(void) {

    // Test VMM page-table frame allocation.
    console_write(
        "[ TEST ] Page-table frame allocation...\n"
    );

    if (!vmm_test_table_frame_allocation()) {
        return vmm_test_failure(
            "Page-table frame allocation test"
        );
    }

    vmm_test_success("Page-table frame allocation test");

    // Report CPU NX capability and current NX state.
    if (vmm_nx_is_supported()) {
        console_write("[ OK ] CPU supports NX\n");
    } else {
        console_write("[ WARNING ] CPU does not support NX\n");
    }

    if (vmm_nx_is_enabled()) {
        console_write("[ OK ] NX protection is enabled\n");
    } else {
        console_write("[ INFO ] NX protection is not enabled\n");
    }

    // Test 1: Inspect a kernel code address.
    vmm_mapping_info_t mapping;

    uint64_t code_address =
        (uint64_t)(uintptr_t)&vmm_init;

    if (vmm_query(code_address, &mapping) != VMM_STATUS_OK) {
        return vmm_test_failure(
            "Kernel code mapping query"
        );
    }

    console_write("Kernel code permissions:\n");

    console_write(mapping.writable
        ? "  Writable: YES\n"
        : "  Writable: NO\n");

    console_write(mapping.user_accessible
        ? "  User accessible: YES\n"
        : "  User accessible: NO\n");

    console_write(mapping.executable
        ? "  Executable: YES\n"
        : "  Executable: NO\n");

    vmm_test_success("Kernel code mapping");

    console_write("Code virtual address: ");
    vmm_test_write_hex(code_address);

    console_write("\nCode physical address: ");
    vmm_test_write_hex(mapping.physical_address);

    console_write("\nCode page size: ");
    vmm_test_write_hex(mapping.page_size);

    console_write("\n");

    // Test 2: Inspect a stack address.
    uint64_t stack_probe = 0;

    uint64_t stack_address =
        (uint64_t)(uintptr_t)&stack_probe;

    if (vmm_query(stack_address, &mapping) != VMM_STATUS_OK) {
        return vmm_test_failure(
            "Kernel stack mapping query"
        );
    }

    console_write("Kernel stack permissions:\n");

    console_write(mapping.writable
        ? "  Writable: YES\n"
        : "  Writable: NO\n");

    console_write(mapping.user_accessible
        ? "  User accessible: YES\n"
        : "  User accessible: NO\n");

    console_write(mapping.executable
        ? "  Executable: YES\n"
        : "  Executable: NO\n");

    vmm_test_success("Kernel stack mapping");

    console_write("Kernel stack raw page-table flags: ");
    vmm_test_write_hex(mapping.flags);

    console_write("\nStack virtual address: ");
    vmm_test_write_hex(stack_address);

    console_write("\nStack physical address: ");
    vmm_test_write_hex(mapping.physical_address);

    console_write("\nStack page size: ");
    vmm_test_write_hex(mapping.page_size);

    console_write("\n");

    /*
     * Test 2 MiB huge-page splitting.
     *
     * The kernel stack is currently mapped using a 2 MiB page.
     * Split that mapping into 512 individual 4 KiB mappings.
     */

    console_write(
        "\n[ TEST ] Splitting kernel stack 2 MiB mapping...\n"
    );

    if (mapping.page_size != VMM_TEST_HUGE_PAGE_SIZE ||
        !mapping.huge_page) {
        return vmm_test_failure(
            "Kernel stack is not a 2 MiB mapping"
        );
    }

    // Save the original physical mapping and permissions.
    uint64_t stack_virtual_base =
        stack_address & ~(VMM_TEST_HUGE_PAGE_SIZE - 1ULL);

    uint64_t stack_physical_base =
        mapping.physical_address -
        (stack_address - stack_virtual_base);

    bool original_writable = mapping.writable;
    bool original_user_accessible = mapping.user_accessible;
    bool original_executable = mapping.executable;

    // Perform the actual page-table split.
    //
    // The VMM splitting API requires the 2 MiB-aligned
    // virtual base of the huge-page mapping.
    vmm_status_t split_status =
        vmm_split_2m(stack_virtual_base);

    if (split_status != VMM_STATUS_OK) {
        console_write("[ FAILED ] 2 MiB page splitting\n");
        console_write("VMM status: ");
        vmm_test_write_hex((uint64_t)split_status);
        console_write("\n");

        return false;
    }

    vmm_test_success("2 MiB page-table split");

    // Verify all 512 resulting 4 KiB mappings.
    bool split_test_passed = true;

    for (uint64_t i = 0;
         i < VMM_TEST_HUGE_PAGE_COUNT;
         i++) {

        uint64_t test_address =
            stack_virtual_base + (i * VMM_TEST_PAGE_SIZE);

        uint64_t expected_physical =
            stack_physical_base + (i * VMM_TEST_PAGE_SIZE);

        vmm_mapping_info_t test_mapping;

        if (vmm_query(test_address, &test_mapping) != VMM_STATUS_OK) {
            split_test_passed = false;
            break;
        }

        if (test_mapping.page_size != VMM_TEST_PAGE_SIZE ||
            test_mapping.huge_page ||
            test_mapping.physical_address != expected_physical ||
            test_mapping.writable != original_writable ||
            test_mapping.user_accessible != original_user_accessible ||
            test_mapping.executable != original_executable) {

            split_test_passed = false;
            break;
        }
    }

    if (!split_test_passed) {
        return vmm_test_failure(
            "512 individual page mapping verification"
        );
    }

    vmm_test_success("All 512 individual 4 KiB mappings");
    vmm_test_success("Physical addresses preserved");
    vmm_test_success("Mapping permissions preserved");

    /*
     * Apply NX protection to the complete kernel stack mapping.
     *
     * The original 2 MiB mapping has already been split into
     * 512 individual 4 KiB mappings.
     */
    bool stack_nx_test_passed = true;

    for (uint64_t i = 0;
         i < VMM_TEST_HUGE_PAGE_COUNT;
         i++) {

        uint64_t page_address =
            stack_virtual_base + (i * VMM_TEST_PAGE_SIZE);

        vmm_status_t nx_status =
            vmm_set_page_executable(page_address, false);

        if (nx_status != VMM_STATUS_OK) {
            stack_nx_test_passed = false;

            console_write(
                "[ FAILED ] Applying NX to kernel stack\n"
            );

            console_write("Page index: ");
            vmm_test_write_hex(i);

            console_write("\nVMM status: ");
            vmm_test_write_hex((uint64_t)nx_status);

            console_write("\n");
            break;
        }
    }

    if (!stack_nx_test_passed) {
        return false;
    }

    vmm_test_success("NX applied to all kernel stack pages");

    /*
     * Verify every stack page is writable, supervisor-only,
     * and non-executable.
     */
    bool stack_permissions_passed = true;

    for (uint64_t i = 0;
         i < VMM_TEST_HUGE_PAGE_COUNT;
         i++) {

        uint64_t page_address =
            stack_virtual_base + (i * VMM_TEST_PAGE_SIZE);

        vmm_mapping_info_t stack_mapping;

        if (vmm_query(page_address, &stack_mapping) != VMM_STATUS_OK) {
            stack_permissions_passed = false;
            break;
        }

        if (stack_mapping.page_size != VMM_TEST_PAGE_SIZE ||
            !stack_mapping.writable ||
            stack_mapping.user_accessible ||
            stack_mapping.executable) {

            stack_permissions_passed = false;
            break;
        }
    }

    if (!stack_permissions_passed) {
        return vmm_test_failure(
            "Kernel stack permission verification"
        );
    }

    vmm_test_success("Kernel stack remains writable");
    vmm_test_success("Kernel stack is supervisor-only");
    vmm_test_success("Kernel stack is non-executable");

    /*
     * Verify kernel code remains read-only,
     * supervisor-only, and executable.
     */
    vmm_mapping_info_t code_mapping;

    if (vmm_query(code_address, &code_mapping) != VMM_STATUS_OK ||
        code_mapping.writable ||
        code_mapping.user_accessible ||
        !code_mapping.executable) {

        return vmm_test_failure(
            "Kernel code permission verification"
        );
    }

    vmm_test_success("Kernel code remains read-only");
    vmm_test_success("Kernel code is supervisor-only");
    vmm_test_success("Kernel code remains executable");

    /*
     * Test invalid arguments and non-canonical addresses
     * for the executable-permission API.
     */
    vmm_mapping_info_t invalid_mapping;

    vmm_status_t invalid_status = vmm_query(
        0x0000800000000000ULL,
        &invalid_mapping
    );

    if (invalid_status != VMM_STATUS_NON_CANONICAL) {
        return vmm_test_failure(
            "VMM non-canonical address handling"
        );
    }

    invalid_status = vmm_set_page_executable(
        0x0000800000000000ULL,
        false
    );

    if (invalid_status != VMM_STATUS_NON_CANONICAL) {
        return vmm_test_failure(
            "NX API non-canonical address handling"
        );
    }

    invalid_status = vmm_set_page_executable(
        code_address + 1,
        false
    );

    if (invalid_status != VMM_STATUS_INVALID_ARGUMENT) {
        return vmm_test_failure(
            "NX API unaligned address rejection"
        );
    }

    vmm_test_success(
        "NX API unaligned address rejection"
    );

    invalid_status = vmm_query(code_address, 0);

    if (invalid_status != VMM_STATUS_INVALID_ARGUMENT) {
        return vmm_test_failure(
            "VMM invalid pointer handling"
        );
    }

    vmm_test_success("Invalid VMM operation handling");

    // Test 3: Reject a non-canonical virtual address.
    uint64_t invalid_address = 0x0000800000000000ULL;

    vmm_status_t status =
        vmm_query(invalid_address, &mapping);

    if (status != VMM_STATUS_NON_CANONICAL) {
        return vmm_test_failure(
            "Non-canonical address rejection"
        );
    }

    vmm_test_success("Non-canonical address rejection");

    // Test 4: Reject a NULL mapping information pointer.
    status = vmm_query(code_address, 0);

    if (status != VMM_STATUS_INVALID_ARGUMENT) {
        return vmm_test_failure(
            "Invalid argument rejection"
        );
    }

    vmm_test_success("Invalid argument rejection");

    // Test 5: Check whether virtual address zero is unmapped.
    status = vmm_query(0, &mapping);

    if (status == VMM_STATUS_NOT_MAPPED) {

        vmm_test_success("Unmapped address detection");

    } else if (status == VMM_STATUS_OK) {

        console_write(
            "[ INFO ] Virtual address zero is mapped\n"
        );

    } else {

        return vmm_test_failure(
            "Unmapped address test"
        );
    }


    /*
     * Test 6: Validate the new single-page mapping API.
     *
     * A dedicated test virtual address is selected from
     * an unused upper-half PML4 region.
     */
    console_write(
        "\n[ TEST ] Single-page virtual memory mapping...\n"
    );

    /*
     * Find an unused virtual address.
     *
     * PML4 index 256 is normally associated with HHDM.
     * Index 511 contains the kernel's higher-half region.
     * We inspect the remaining upper-half slots instead
     * of assuming that an arbitrary address is available.
     */
    uint64_t test_virtual_address = 0;
    bool test_address_found = false;

    for (uint64_t index = 257; index <= 510; index++) {

        uint64_t candidate =
            0xFFFF000000000000ULL |
            (index << 39);

        vmm_mapping_info_t candidate_mapping;

        vmm_status_t candidate_status =
            vmm_query(candidate, &candidate_mapping);

        if (candidate_status == VMM_STATUS_NOT_MAPPED) {
            test_virtual_address = candidate;
            test_address_found = true;
            break;
        }
    }

    if (!test_address_found) {
        return vmm_test_failure(
            "Finding an unused test virtual address"
        );
    }

    /*
     * Allocate a physical data frame.
     *
     * This frame belongs to the test, not to VMM.
     */
    uint64_t test_physical_address =
        pmm_alloc_frame();

    if (test_physical_address == 0) {
        return vmm_test_failure(
            "Allocating physical frame for mapping test"
        );
    }

    /*
     * Map the frame as:
     *
     * Writable
     * Supervisor-only
     * Non-executable
     */
    vmm_status_t map_status =
        vmm_map_page(
            test_virtual_address,
            test_physical_address,
            VMM_MAP_WRITABLE
        );

    if (map_status != VMM_STATUS_OK) {

        /*
         * Mapping failed, so the frame has not become
         * reachable through this virtual address.
         */
        pmm_free_frame(test_physical_address);

        console_write("Mapping status: ");
        vmm_test_write_hex((uint64_t)map_status);
        console_write("\n");

        return vmm_test_failure(
            "Creating a 4 KiB virtual mapping"
        );
    }

    vmm_test_success("Creating a 4 KiB virtual mapping");

    /*
     * Verify the resulting mapping.
     */
    vmm_mapping_info_t test_mapping;

    if (vmm_query(
            test_virtual_address,
            &test_mapping
        ) != VMM_STATUS_OK) {

        return vmm_test_failure(
            "Querying newly created mapping"
        );
    }

    if (test_mapping.physical_address != test_physical_address ||
        test_mapping.page_size != VMM_TEST_PAGE_SIZE ||
        test_mapping.huge_page) {

        return vmm_test_failure(
            "Physical address and page-size verification"
        );
    }

    vmm_test_success("Physical address correctly mapped");
    vmm_test_success("4 KiB page size verified");

    /*
     * Verify effective permissions.
     */
    if (!test_mapping.writable ||
        test_mapping.user_accessible ||
        test_mapping.executable) {

        return vmm_test_failure(
            "New mapping permission verification"
        );
    }

    vmm_test_success("Writable permission verified");
    vmm_test_success("Supervisor-only permission verified");
    vmm_test_success("NX protection verified");

    /*
     * Verify that the CPU can actually access the mapped
     * physical frame through the new virtual address.
     */
    volatile uint64_t *test_virtual_pointer =
        (volatile uint64_t *)(uintptr_t)test_virtual_address;

    const uint64_t test_pattern =
        0x4855415F564D4D31ULL;

    *test_virtual_pointer = test_pattern;

    if (*test_virtual_pointer != test_pattern) {
        return vmm_test_failure(
            "Writing and reading through virtual mapping"
        );
    }

    vmm_test_success("Virtual memory read/write test");

    /*
     * Attempt to map the same virtual address again.
     *
     * Existing mappings must never be silently replaced.
     */
    map_status =
        vmm_map_page(
            test_virtual_address,
            test_physical_address,
            VMM_MAP_WRITABLE
        );

    if (map_status != VMM_STATUS_ALREADY_MAPPED) {
        return vmm_test_failure(
            "Rejecting an already mapped virtual address"
        );
    }

    vmm_test_success("Already-mapped address rejection");

    /*
     * Test invalid mapping arguments.
     */
    if (vmm_map_page(
            test_virtual_address + 1,
            test_physical_address,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Rejecting unaligned virtual address"
        );
    }

    if (vmm_map_page(
            test_virtual_address,
            test_physical_address + 1,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Rejecting unaligned physical address"
        );
    }

    if (vmm_map_page(
            0x0000800000000000ULL,
            test_physical_address,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_NON_CANONICAL) {

        return vmm_test_failure(
            "Rejecting non-canonical mapping address"
        );
    }

    if (vmm_map_page(
            test_virtual_address,
            test_physical_address,
            (1ULL << 20)
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Rejecting unsupported mapping flags"
        );
    }

    vmm_test_success("Invalid mapping argument rejection");

    /*
     * Test W^X enforcement.
     *
     * Writable and executable permissions must never
     * be enabled simultaneously for one mapping.
     */

    uint64_t free_frames_before_wx = pmm_get_free_frames();

    if (vmm_map_page(
            test_virtual_address + 0x1000,
            test_physical_address,
            VMM_MAP_WRITABLE | VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Rejecting writable and executable mapping"
        );
    }

    /*
     * Verify that rejection did not consume PMM frames.
     */
    if (pmm_get_free_frames() != free_frames_before_wx) {
        return vmm_test_failure(
            "W^X rejection does not leak physical frames"
        );
    }

    vmm_test_success("W^X rejection preserves PMM frame count");

    /*
     * Verify that the rejected mapping was not installed.
     */
    vmm_mapping_info_t wx_mapping;

    if (vmm_query(
            test_virtual_address + 0x1000,
            &wx_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return vmm_test_failure(
            "W^X rejection leaves virtual address unmapped"
        );
    }

    vmm_test_success("W^X mapping protection");

    /*
     * Test invalid unmapping arguments.
     */
    if (vmm_unmap_page(
            test_virtual_address,
            0
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Rejecting null unmap output pointer"
        );
    }

    if (vmm_unmap_page(
            test_virtual_address + 1,
            &(uint64_t){0}
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Rejecting unaligned unmap address"
        );
    }

    if (vmm_unmap_page(
            0x0000800000000000ULL,
            &(uint64_t){0}
        ) != VMM_STATUS_NON_CANONICAL) {

        return vmm_test_failure(
            "Rejecting non-canonical unmap address"
        );
    }

    vmm_test_success("Invalid unmapping argument rejection");

    /*
     * Remove the mapping and retrieve its physical frame.
     */
    uint64_t unmapped_physical_address = 0;

    vmm_status_t unmap_status =
        vmm_unmap_page(
            test_virtual_address,
            &unmapped_physical_address
        );

    if (unmap_status != VMM_STATUS_OK) {

        console_write("Unmapping status: ");
        vmm_test_write_hex((uint64_t)unmap_status);
        console_write("\n");

        return vmm_test_failure(
            "Removing a 4 KiB virtual mapping"
        );
    }

    vmm_test_success("Removing a 4 KiB virtual mapping");

    /*
     * Verify that the returned physical address is correct.
     */
    if (unmapped_physical_address != test_physical_address) {
        return vmm_test_failure(
            "Unmapped physical address verification"
        );
    }

    vmm_test_success("Unmapped physical address correctly returned");

    /*
     * Verify that the virtual address is no longer mapped.
     */
    if (vmm_query(
            test_virtual_address,
            &test_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return vmm_test_failure(
            "Verifying mapping removal"
        );
    }

    vmm_test_success("Removed mapping is no longer accessible through VMM");

    /*
     * Attempt to unmap the same address again.
     */
    unmapped_physical_address = UINT64_MAX;

    unmap_status =
        vmm_unmap_page(
            test_virtual_address,
            &unmapped_physical_address
        );

    if (unmap_status != VMM_STATUS_NOT_MAPPED ||
        unmapped_physical_address != 0) {

        return vmm_test_failure(
            "Rejecting an already unmapped address"
        );
    }

    vmm_test_success("Already-unmapped address rejection");

    /*
     * Test page-table reclamation and recreation.
     *
     * The previous unmap should have reclaimed empty
     * VMM-owned intermediate page tables.
     *
     * Mapping the same virtual address again exercises
     * their recreation.
     */
    map_status =
        vmm_map_page(
            test_virtual_address,
            test_physical_address,
            VMM_MAP_WRITABLE
        );

    if (map_status != VMM_STATUS_OK) {

        console_write("Remapping status: ");
        vmm_test_write_hex((uint64_t)map_status);
        console_write("\n");

        return vmm_test_failure(
            "Recreating reclaimed page tables"
        );
    }

    vmm_test_success("Recreating reclaimed page tables");

    /*
     * Remove the recreated mapping.
     */
    unmapped_physical_address = 0;

    unmap_status =
        vmm_unmap_page(
            test_virtual_address,
            &unmapped_physical_address
        );

    if (unmap_status != VMM_STATUS_OK ||
        unmapped_physical_address != test_physical_address) {

        return vmm_test_failure(
            "Unmapping recreated page tables"
        );
    }

    vmm_test_success("Recreated mapping successfully unmapped");

    /*
     * The mapping is gone, so the caller can now free
     * the physical data frame.
     *
     * VMM does not own this data frame.
     */
    if (!pmm_free_frame(unmapped_physical_address)) {
        return vmm_test_failure(
            "Freeing unmapped physical data frame"
        );
    }

    vmm_test_success("Unmapped physical frame successfully freed");

    /*
     * TEST: Page-table allocation failure and rollback.
     *
     * The successful mapping tests above have already initialized
     * the VMM ownership registry.
     *
     * We deliberately fail the third intermediate page-table
     * allocation after allowing two successful allocations.
     *
     * Expected behavior:
     *
     * 1. First page-table allocation succeeds.
     * 2. Second page-table allocation succeeds.
     * 3. Third page-table allocation fails.
     * 4. VMM rolls back the second table.
     * 5. VMM rolls back the first table.
     * 6. PMM free-frame accounting returns to its baseline.
     */

    console_write(
        "\n[ TEST ] Page-table allocation failure rollback...\n"
    );

    /*
     * Allocate a physical data frame owned by this test.
     */
    uint64_t rollback_test_physical =
        pmm_alloc_frame();

    if (rollback_test_physical == 0) {
        return vmm_test_failure(
            "Allocating rollback test data frame"
        );
    }

    /*
     * The test virtual address must currently be unmapped.
     */
    vmm_mapping_info_t rollback_mapping;

    if (vmm_query(
            test_virtual_address,
            &rollback_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        pmm_free_frame(rollback_test_physical);

        return vmm_test_failure(
            "Rollback test virtual address is unmapped"
        );
    }

    /*
     * Record PMM availability after allocating the data frame.
     *
     * The two temporary page-table frames must be returned
     * after the forced failure.
     */
    uint64_t rollback_free_frames_before =
        pmm_get_free_frames();

    /*
     * Permit two successful page-table allocations.
     * Force the third allocation to fail.
     */
    vmm_test_fail_table_allocation_after(2);

    vmm_status_t rollback_status =
        vmm_map_page(
            test_virtual_address,
            rollback_test_physical,
            VMM_MAP_WRITABLE
        );

    /*
     * Always disable the test hook after the operation.
     */
    vmm_test_clear_table_allocation_failure();

    /*
     * The third page-table allocation must report out of memory.
     */
    if (rollback_status != VMM_STATUS_OUT_OF_MEMORY) {

        console_write("Unexpected mapping status: ");
        vmm_test_write_hex((uint64_t)rollback_status);
        console_write("\n");

        /*
         * If the mapping unexpectedly succeeded, remove it
         * before freeing the caller-owned physical frame.
         */
        if (rollback_status == VMM_STATUS_OK) {
            uint64_t removed_physical = 0;

            if (vmm_unmap_page(
                    test_virtual_address,
                    &removed_physical
                ) == VMM_STATUS_OK &&
                removed_physical == rollback_test_physical) {

                pmm_free_frame(removed_physical);
            }
        } else {
            pmm_free_frame(rollback_test_physical);
        }

        return vmm_test_failure(
            "Forcing page-table allocation failure"
        );
    }

    vmm_test_success("Controlled page-table allocation failure");

    /*
     * Verify that no mapping was installed.
     */
    if (vmm_query(
            test_virtual_address,
            &rollback_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        pmm_free_frame(rollback_test_physical);

        return vmm_test_failure(
            "Failed mapping leaves virtual address unmapped"
        );
    }

    vmm_test_success("Failed mapping leaves no partial mapping");

    /*
     * Verify that both intermediate page-table frames
     * were reclaimed.
     */
    if (pmm_get_free_frames() != rollback_free_frames_before) {

        console_write("Frames before rollback: ");
        vmm_test_write_hex(rollback_free_frames_before);

        console_write("\nFrames after rollback: ");
        vmm_test_write_hex(pmm_get_free_frames());

        console_write("\n");

        pmm_free_frame(rollback_test_physical);

        return vmm_test_failure(
            "Page-table rollback restores PMM frame count"
        );
    }

    vmm_test_success("Page-table rollback restores PMM frame count");

    /*
     * The physical data frame was never owned by VMM.
     * The test must release it separately.
     */
    if (!pmm_free_frame(rollback_test_physical)) {
        return vmm_test_failure(
            "Freeing rollback test data frame"
        );
    }

    vmm_test_success("Rollback test data frame released");

    /*
    * TEST: Failure at the first and second page-table allocations.
    *
    * failure_point = 0:
    *   The first allocation fails.
    *   No page-table frame should be allocated.
    *
    * failure_point = 1:
    *   The first allocation succeeds.
    *   The second allocation fails.
    *   The first newly created table must be reclaimed.
    */

    console_write(
        "\n[ TEST ] First and second page-table allocation failures...\n"
    );

    for (uint64_t failure_point = 0;
        failure_point < 2;
        failure_point++) {

        uint64_t failure_test_physical = pmm_alloc_frame();

        if (failure_test_physical == 0) {
            return vmm_test_failure(
                "Allocating page-table failure test data frame"
            );
        }

        vmm_mapping_info_t failure_mapping;

        if (vmm_query(
                test_virtual_address,
                &failure_mapping
            ) != VMM_STATUS_NOT_MAPPED) {

            pmm_free_frame(failure_test_physical);

            return vmm_test_failure(
                "Failure test virtual address is unmapped"
            );
        }

        /*
        * Record available frames after allocating the data frame.
        */
        uint64_t frames_before =
            pmm_get_free_frames();

        /*
        * Allow zero or one successful page-table allocations.
        * Then force the next allocation to fail.
        */
        vmm_test_fail_table_allocation_after(failure_point);

        vmm_status_t failure_status =
            vmm_map_page(
                test_virtual_address,
                failure_test_physical,
                VMM_MAP_WRITABLE
            );

        vmm_test_clear_table_allocation_failure();

        /*
        * The mapping must fail with OUT_OF_MEMORY.
        */
        if (failure_status != VMM_STATUS_OUT_OF_MEMORY) {

            console_write("Unexpected mapping status: ");
            vmm_test_write_hex((uint64_t)failure_status);
            console_write("\n");

            if (failure_status == VMM_STATUS_OK) {
                uint64_t removed_physical = 0;

                if (vmm_unmap_page(
                        test_virtual_address,
                        &removed_physical
                    ) == VMM_STATUS_OK &&
                    removed_physical == failure_test_physical) {

                    pmm_free_frame(removed_physical);
                }
            } else {
                pmm_free_frame(failure_test_physical);
            }

            return vmm_test_failure(
                "First or second page-table allocation failure"
            );
        }

        if (failure_point == 0) {
            vmm_test_success(
                "First page-table allocation failure"
            );
        } else {
            vmm_test_success(
                "Second page-table allocation failure"
            );
        }

        /*
        * Verify that no virtual mapping was installed.
        */
        if (vmm_query(
                test_virtual_address,
                &failure_mapping
            ) != VMM_STATUS_NOT_MAPPED) {

            pmm_free_frame(failure_test_physical);

            return vmm_test_failure(
                "Allocation failure leaves no mapping"
            );
        }

        /*
        * Verify that all temporary page-table frames
        * were reclaimed.
        */
        if (pmm_get_free_frames() != frames_before) {

            console_write("Frames before rollback: ");
            vmm_test_write_hex(frames_before);

            console_write("\nFrames after rollback: ");
            vmm_test_write_hex(pmm_get_free_frames());

            console_write("\n");

            pmm_free_frame(failure_test_physical);

            return vmm_test_failure(
                "First or second allocation rollback restores PMM"
            );
        }

        if (failure_point == 0) {
            vmm_test_success(
                "First allocation preserves PMM frame count"
            );
        } else {
            vmm_test_success(
                "Second allocation rollback restores PMM frame count"
            );
        }

        /*
        * Release the caller-owned data frame.
        */
        if (!pmm_free_frame(failure_test_physical)) {
            return vmm_test_failure(
                "Releasing allocation failure test data frame"
            );
        }
    }

    vmm_test_success(
        "First and second allocation failure tests"
    );

    /*
     * TEST: Map three consecutive physical frames
     * into three consecutive virtual pages.
     */
    console_write(
        "\n[ TEST ] Three-page range mapping...\n"
    );

    uint64_t range_physical_base = 0;

    if (!vmm_test_alloc_contiguous_frames(
            3,
            &range_physical_base
        )) {

        return vmm_test_failure(
            "Allocating three contiguous physical frames"
        );
    }

    vmm_test_success(
        "Three contiguous physical frames allocated"
    );

    /*
     * Map all three pages.
     */
    vmm_status_t range_status =
        vmm_map_range(
            test_virtual_address,
            range_physical_base,
            3,
            VMM_MAP_WRITABLE
        );

    if (range_status != VMM_STATUS_OK) {

        console_write("Range mapping status: ");
        vmm_test_write_hex((uint64_t)range_status);
        console_write("\n");

        /*
         * Do not release frames if any mapping remains.
         */
        bool cleanup_safe = true;

        for (uint64_t i = 0; i < 3; i++) {

            vmm_mapping_info_t remaining_mapping;

            if (vmm_query(
                    test_virtual_address +
                        i * VMM_TEST_PAGE_SIZE,
                    &remaining_mapping
                ) == VMM_STATUS_OK) {

                uint64_t removed_physical = 0;

                if (vmm_unmap_page(
                        test_virtual_address +
                            i * VMM_TEST_PAGE_SIZE,
                        &removed_physical
                    ) != VMM_STATUS_OK ||
                    removed_physical !=
                        range_physical_base +
                            i * VMM_TEST_PAGE_SIZE) {

                    cleanup_safe = false;
                }
            }
        }

        if (cleanup_safe) {
            for (uint64_t i = 0; i < 3; i++) {
                pmm_free_frame(
                    range_physical_base +
                        i * VMM_TEST_PAGE_SIZE
                );
            }
        }

        return vmm_test_failure(
            "Three-page range mapping"
        );
    }

    vmm_test_success(
        "Three-page range mapping"
    );

    /*
     * Verify every virtual-to-physical translation.
     */
    bool range_queries_valid = true;

    for (uint64_t i = 0; i < 3; i++) {

        uint64_t current_virtual =
            test_virtual_address +
            i * VMM_TEST_PAGE_SIZE;

        uint64_t expected_physical =
            range_physical_base +
            i * VMM_TEST_PAGE_SIZE;

        vmm_mapping_info_t range_mapping;

        if (vmm_query(
                current_virtual,
                &range_mapping
            ) != VMM_STATUS_OK ||
            range_mapping.physical_address !=
                expected_physical ||
            range_mapping.page_size !=
                VMM_TEST_PAGE_SIZE ||
            !range_mapping.writable) {

            range_queries_valid = false;
            break;
        }
    }

    if (!range_queries_valid) {

        /*
         * Remove the mappings before releasing data frames.
         */
        bool cleanup_safe = true;

        for (uint64_t i = 0; i < 3; i++) {

            uint64_t removed_physical = 0;

            if (vmm_unmap_page(
                    test_virtual_address +
                        i * VMM_TEST_PAGE_SIZE,
                    &removed_physical
                ) != VMM_STATUS_OK ||
                removed_physical !=
                    range_physical_base +
                        i * VMM_TEST_PAGE_SIZE) {

                cleanup_safe = false;
            }
        }

        if (cleanup_safe) {
            for (uint64_t i = 0; i < 3; i++) {
                pmm_free_frame(
                    range_physical_base +
                        i * VMM_TEST_PAGE_SIZE
                );
            }
        }

        return vmm_test_failure(
            "Three-page physical mapping verification"
        );
    }

    vmm_test_success(
        "All three physical mappings verified"
    );

    /*
     * Unmap each page and verify the returned
     * physical address.
     */
    bool range_unmap_valid = true;

    for (uint64_t i = 0; i < 3; i++) {

        uint64_t removed_physical = 0;

        vmm_status_t unmap_status =
            vmm_unmap_page(
                test_virtual_address +
                    i * VMM_TEST_PAGE_SIZE,
                &removed_physical
            );

        if (unmap_status != VMM_STATUS_OK ||
            removed_physical !=
                range_physical_base +
                    i * VMM_TEST_PAGE_SIZE) {

            range_unmap_valid = false;
            break;
        }
    }

    if (!range_unmap_valid) {
        return vmm_test_failure(
            "Three-page range unmapping"
        );
    }

    vmm_test_success(
        "Three-page range unmapping"
    );

    /*
     * Physical frames are still caller-owned.
     * Release them explicitly through PMM.
     */
    for (uint64_t i = 0; i < 3; i++) {

        if (!pmm_free_frame(
                range_physical_base +
                    i * VMM_TEST_PAGE_SIZE
            )) {

            return vmm_test_failure(
                "Releasing range-mapping data frames"
            );
        }
    }

    vmm_test_success(
        "Three range-mapping data frames released"
    );


    /*
     * =====================================================
     * STEP 13B: COMPREHENSIVE RANGE-MAPPING TESTS
     * =====================================================
     */

    /*
     * TEST 1: Mapping across a 2 MiB page-table boundary.
     *
     * The first page occupies the final PTE in one page
     * table. The second page requires another page table.
     */

    console_write(
        "\n[ TEST ] Mapping across 2 MiB boundary...\n"
    );

    uint64_t boundary_virtual =
        test_virtual_address +
        VMM_TEST_HUGE_PAGE_SIZE -
        VMM_TEST_PAGE_SIZE;

    uint64_t boundary_physical = 0;

    if (!vmm_test_alloc_contiguous_frames(
            2,
            &boundary_physical
        )) {

        return vmm_test_failure(
            "Allocating boundary-test physical frames"
        );
    }

    if (vmm_query(
            boundary_virtual,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_query(
            boundary_virtual + VMM_TEST_PAGE_SIZE,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        vmm_test_release_range(
            boundary_virtual,
            boundary_physical,
            2
        );

        return vmm_test_failure(
            "Boundary-test virtual addresses are unmapped"
        );
    }

    vmm_status_t boundary_status =
        vmm_map_range(
            boundary_virtual,
            boundary_physical,
            2,
            VMM_MAP_WRITABLE
        );

    if (boundary_status != VMM_STATUS_OK) {

        console_write("Boundary mapping status: ");
        vmm_test_write_hex((uint64_t)boundary_status);
        console_write("\n");

        vmm_test_release_range(
            boundary_virtual,
            boundary_physical,
            2
        );

        return vmm_test_failure(
            "Mapping across 2 MiB boundary"
        );
    }

    bool boundary_valid = true;

    for (uint64_t i = 0; i < 2; i++) {

        uint64_t current_virtual =
            boundary_virtual +
            i * VMM_TEST_PAGE_SIZE;

        uint64_t expected_physical =
            boundary_physical +
            i * VMM_TEST_PAGE_SIZE;

        vmm_mapping_info_t boundary_mapping;

        if (vmm_query(
                current_virtual,
                &boundary_mapping
            ) != VMM_STATUS_OK ||
            boundary_mapping.physical_address !=
                expected_physical ||
            boundary_mapping.page_size !=
                VMM_TEST_PAGE_SIZE ||
            !boundary_mapping.writable) {

            boundary_valid = false;
            break;
        }
    }

    if (!boundary_valid) {

        vmm_test_release_range(
            boundary_virtual,
            boundary_physical,
            2
        );

        return vmm_test_failure(
            "2 MiB boundary mapping verification"
        );
    }

    vmm_test_success(
        "Mapping across 2 MiB boundary"
    );

    if (!vmm_test_release_range(
            boundary_virtual,
            boundary_physical,
            2
        )) {

        return vmm_test_failure(
            "Boundary mapping cleanup"
        );
    }

    vmm_test_success(
        "2 MiB boundary mappings removed and frames released"
    );


    /*
     * TEST 2: Reject a range containing an existing mapping.
     *
     * Only the second virtual page is mapped before
     * calling vmm_map_range().
     *
     * The range operation must reject the entire request
     * without installing the first page.
     */

    console_write(
        "\n[ TEST ] Existing mapping rejection...\n"
    );

    uint64_t overlap_virtual =
        test_virtual_address +
        4 * VMM_TEST_HUGE_PAGE_SIZE;

    uint64_t overlap_physical = 0;

    if (!vmm_test_alloc_contiguous_frames(
            2,
            &overlap_physical
        )) {

        return vmm_test_failure(
            "Allocating existing-mapping test frames"
        );
    }

    vmm_status_t overlap_setup_status =
        vmm_map_page(
            overlap_virtual + VMM_TEST_PAGE_SIZE,
            overlap_physical + VMM_TEST_PAGE_SIZE,
            VMM_MAP_WRITABLE
        );

    if (overlap_setup_status != VMM_STATUS_OK) {

        vmm_test_release_range(
            overlap_virtual,
            overlap_physical,
            2
        );

        return vmm_test_failure(
            "Creating existing mapping for overlap test"
        );
    }

    uint64_t overlap_frames_before =
        pmm_get_free_frames();

    vmm_status_t overlap_status =
        vmm_map_range(
            overlap_virtual,
            overlap_physical,
            2,
            VMM_MAP_WRITABLE
        );

    vmm_mapping_info_t overlap_mapping;

    bool first_page_unmapped =
        vmm_query(
            overlap_virtual,
            &overlap_mapping
        ) == VMM_STATUS_NOT_MAPPED;

    bool second_page_preserved =
        vmm_query(
            overlap_virtual + VMM_TEST_PAGE_SIZE,
            &overlap_mapping
        ) == VMM_STATUS_OK &&
        overlap_mapping.physical_address ==
            overlap_physical + VMM_TEST_PAGE_SIZE;

    bool overlap_frames_preserved =
        pmm_get_free_frames() == overlap_frames_before;

    if (overlap_status != VMM_STATUS_ALREADY_MAPPED ||
        !first_page_unmapped ||
        !second_page_preserved ||
        !overlap_frames_preserved) {

        vmm_test_release_range(
            overlap_virtual,
            overlap_physical,
            2
        );

        return vmm_test_failure(
            "Existing mapping rejection and preservation"
        );
    }

    vmm_test_success(
        "Existing mapping rejects complete range"
    );

    vmm_test_success(
        "Existing mapping remains unchanged"
    );

    if (!vmm_test_release_range(
            overlap_virtual,
            overlap_physical,
            2
        )) {

        return vmm_test_failure(
            "Existing-mapping test cleanup"
        );
    }

    vmm_test_success(
        "Existing-mapping test frames released"
    );


    /*
     * TEST 3: Invalid arguments and address validation.
     *
     * Each request must fail before installing mappings.
     */

    console_write(
        "\n[ TEST ] Range argument and address validation...\n"
    );

    uint64_t crossing_start =
        0x00007FFFFFFFF000ULL;

    uint64_t crossing_end =
        0xFFFF800000000000ULL;

    uint64_t crossing_page_count =
        ((crossing_end - crossing_start) /
            VMM_TEST_PAGE_SIZE) + 1;

    struct {
        const char *name;
        uint64_t virtual_address;
        uint64_t physical_address;
        uint64_t page_count;
        uint64_t flags;
        vmm_status_t expected_status;
    } invalid_range_tests[] = {

        {
            "Zero page count",
            test_virtual_address,
            0,
            0,
            VMM_MAP_WRITABLE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Unaligned virtual address",
            test_virtual_address + 1,
            0,
            1,
            VMM_MAP_WRITABLE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Unaligned physical address",
            test_virtual_address + 0x1000000ULL,
            1,
            1,
            VMM_MAP_WRITABLE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Non-canonical starting address",
            0x0000800000000000ULL,
            0,
            1,
            VMM_MAP_WRITABLE,
            VMM_STATUS_NON_CANONICAL
        },

        {
            "Virtual range arithmetic overflow",
            UINT64_MAX - (VMM_TEST_PAGE_SIZE - 1),
            0,
            2,
            VMM_MAP_WRITABLE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Physical range arithmetic overflow",
            test_virtual_address + 0x2000000ULL,
            UINT64_MAX - (VMM_TEST_PAGE_SIZE - 1),
            2,
            VMM_MAP_WRITABLE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Page-count multiplication overflow",
            test_virtual_address + 0x3000000ULL,
            0,
            UINT64_MAX,
            VMM_MAP_WRITABLE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Range crossing canonical address gap",
            crossing_start,
            0,
            crossing_page_count,
            VMM_MAP_WRITABLE,
            VMM_STATUS_NON_CANONICAL
        },

        {
            "Writable and executable mapping rejected",
            test_virtual_address + 0x4000000ULL,
            0,
            1,
            VMM_MAP_WRITABLE | VMM_MAP_EXECUTABLE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Unsupported mapping flags rejected",
            test_virtual_address + 0x5000000ULL,
            0,
            1,
            1ULL << 20,
            VMM_STATUS_INVALID_ARGUMENT
        }
    };

    uint64_t invalid_test_count =
        sizeof(invalid_range_tests) /
        sizeof(invalid_range_tests[0]);

    for (uint64_t i = 0;
         i < invalid_test_count;
         i++) {

        vmm_status_t actual_status =
            vmm_map_range(
                invalid_range_tests[i].virtual_address,
                invalid_range_tests[i].physical_address,
                invalid_range_tests[i].page_count,
                invalid_range_tests[i].flags
            );

        if (actual_status !=
            invalid_range_tests[i].expected_status) {

            console_write("Failed validation test: ");
            console_write(invalid_range_tests[i].name);

            console_write("\nExpected status: ");
            vmm_test_write_hex(
                (uint64_t)
                invalid_range_tests[i].expected_status
            );

            console_write("\nActual status: ");
            vmm_test_write_hex((uint64_t)actual_status);

            console_write("\n");

            return vmm_test_failure(
                "Range argument and address validation"
            );
        }
    }

    vmm_test_success(
        "All range argument and address validation tests"
    );


    /*
     * TEST 4: Allocation failure during the second page.
     *
     * Start at the last 4 KiB page of a 2 MiB region.
     *
     * First page:
     *   Requires three intermediate page-table allocations.
     *
     * Second page:
     *   Requires another page-table allocation because
     *   it belongs to the next 2 MiB region.
     *
     * Allow the first three allocations and fail the fourth.
     * The first page must then be rolled back.
     */

    console_write(
        "\n[ TEST ] Range mapping allocation-failure rollback...\n"
    );

    uint64_t rollback_range_virtual =
        boundary_virtual;

    uint64_t rollback_range_physical = 0;

    if (!vmm_test_alloc_contiguous_frames(
            2,
            &rollback_range_physical
        )) {

        return vmm_test_failure(
            "Allocating range rollback test frames"
        );
    }

    if (vmm_query(
            rollback_range_virtual,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_query(
            rollback_range_virtual + VMM_TEST_PAGE_SIZE,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        vmm_test_release_range(
            rollback_range_virtual,
            rollback_range_physical,
            2
        );

        return vmm_test_failure(
            "Range rollback test addresses are unmapped"
        );
    }

    uint64_t rollback_frames_before =
        pmm_get_free_frames();

    /*
     * Permit three successful page-table allocations.
     * Force the next allocation to fail.
     */
    vmm_test_fail_table_allocation_after(3);

    vmm_status_t rollback_range_status =
        vmm_map_range(
            rollback_range_virtual,
            rollback_range_physical,
            2,
            VMM_MAP_WRITABLE
        );

    vmm_test_clear_table_allocation_failure();

    if (rollback_range_status != VMM_STATUS_OUT_OF_MEMORY) {

        console_write("Unexpected range status: ");
        vmm_test_write_hex(
            (uint64_t)rollback_range_status
        );
        console_write("\n");

        vmm_test_release_range(
            rollback_range_virtual,
            rollback_range_physical,
            2
        );

        return vmm_test_failure(
            "Range mapping allocation failure"
        );
    }

    vmm_test_success(
        "Second page-table allocation failure"
    );

    /*
     * Verify that neither page remains mapped.
     */
    bool rollback_range_unmapped = true;

    for (uint64_t i = 0; i < 2; i++) {

        uint64_t current_virtual =
            rollback_range_virtual +
            i * VMM_TEST_PAGE_SIZE;

        if (vmm_query(
                current_virtual,
                &mapping
            ) != VMM_STATUS_NOT_MAPPED) {

            rollback_range_unmapped = false;
            break;
        }
    }

    if (!rollback_range_unmapped) {

        vmm_test_release_range(
            rollback_range_virtual,
            rollback_range_physical,
            2
        );

        return vmm_test_failure(
            "Failed range mapping leaves no partial mappings"
        );
    }

    vmm_test_success(
        "Failed range mapping leaves no partial mappings"
    );

    /*
     * Verify that temporary page-table allocations
     * have been reclaimed.
     */
    if (pmm_get_free_frames() != rollback_frames_before) {

        console_write("Frames before rollback: ");
        vmm_test_write_hex(rollback_frames_before);

        console_write("\nFrames after rollback: ");
        vmm_test_write_hex(pmm_get_free_frames());

        console_write("\n");

        vmm_test_release_range(
            rollback_range_virtual,
            rollback_range_physical,
            2
        );

        return vmm_test_failure(
            "Range rollback restores PMM frame count"
        );
    }

    vmm_test_success(
        "Range rollback restores PMM frame count"
    );

    /*
     * Release the caller-owned data frames.
     */
    if (!vmm_test_release_range(
            rollback_range_virtual,
            rollback_range_physical,
            2
        )) {

        return vmm_test_failure(
            "Releasing range rollback test frames"
        );
    }

    vmm_test_success(
        "Range rollback test frames released"
    );


    /*
     * All Step 13B tests completed.
     */
    vmm_test_success(
        "VMM range-mapping comprehensive tests"
    );


    /*
     * ============================================================
     * STEP 13C: COMPREHENSIVE RANGE-UNMAPPING TESTS
     * ============================================================
     */

    /*
     * TEST 1:
     * Successfully unmap three consecutive pages.
     */
    uint64_t unmap_range_virtual =
        test_virtual_address + (8ULL * VMM_TEST_HUGE_PAGE_SIZE);

    uint64_t unmap_range_physical = 0;

    if (!vmm_test_alloc_contiguous_frames(
            3,
            &unmap_range_physical
        )) {
        return vmm_test_failure(
            "Allocating three range-unmapping test frames"
        );
    }

    for (uint64_t i = 0; i < 3; i++) {
        if (vmm_query(
                unmap_range_virtual + i * VMM_TEST_PAGE_SIZE,
                &mapping
            ) != VMM_STATUS_NOT_MAPPED) {

            vmm_test_release_range(
                unmap_range_virtual,
                unmap_range_physical,
                3
            );

            return vmm_test_failure(
                "Three-page unmap test addresses are unmapped"
            );
        }
    }

    uint64_t unmap_frames_before =
        pmm_get_free_frames();

    if (vmm_map_range(
            unmap_range_virtual,
            unmap_range_physical,
            3,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        vmm_test_release_range(
            unmap_range_virtual,
            unmap_range_physical,
            3
        );

        return vmm_test_failure(
            "Preparing three-page range-unmapping test"
        );
    }

    vmm_test_success(
        "Three-page range prepared for unmapping"
    );

    uint64_t returned_physical_addresses[3] = {0};

    vmm_status_t unmap_range_status =
        vmm_unmap_range(
            unmap_range_virtual,
            3,
            returned_physical_addresses
        );

    if (unmap_range_status != VMM_STATUS_OK) {

        console_write("Unexpected unmap status: ");
        vmm_test_write_hex((uint64_t)unmap_range_status);
        console_write("\n");

        vmm_test_release_range(
            unmap_range_virtual,
            unmap_range_physical,
            3
        );

        return vmm_test_failure(
            "Three-page range unmapping"
        );
    }

    vmm_test_success(
        "Three-page range unmapping"
    );

    /*
     * Verify every returned physical address.
     */
    bool returned_addresses_correct = true;

    for (uint64_t i = 0; i < 3; i++) {

        uint64_t expected_physical =
            unmap_range_physical +
            i * VMM_TEST_PAGE_SIZE;

        if (returned_physical_addresses[i] !=
            expected_physical) {

            returned_addresses_correct = false;
            break;
        }
    }

    if (!returned_addresses_correct) {
        return vmm_test_failure(
            "Range unmapping returns correct physical addresses"
        );
    }

    vmm_test_success(
        "Range unmapping returns correct physical addresses"
    );

    /*
     * Verify that all virtual pages are now unmapped.
     */
    bool all_range_pages_unmapped = true;

    for (uint64_t i = 0; i < 3; i++) {

        if (vmm_query(
                unmap_range_virtual +
                    i * VMM_TEST_PAGE_SIZE,
                &mapping
            ) != VMM_STATUS_NOT_MAPPED) {

            all_range_pages_unmapped = false;
            break;
        }
    }

    if (!all_range_pages_unmapped) {
        return vmm_test_failure(
            "All three pages become unmapped"
        );
    }

    vmm_test_success(
        "All three pages become unmapped"
    );

    /*
     * Empty page-table reclamation should restore the
     * PMM count to the value before mapping.
     */
    if (pmm_get_free_frames() != unmap_frames_before) {

        console_write("Frames before mapping: ");
        vmm_test_write_hex(unmap_frames_before);

        console_write("\nFrames after unmapping: ");
        vmm_test_write_hex(pmm_get_free_frames());

        console_write("\n");

        return vmm_test_failure(
            "Range unmapping reclaims empty page tables"
        );
    }

    vmm_test_success(
        "Range unmapping reclaims empty page tables"
    );

    /*
     * Caller-owned physical data frames must be released
     * separately from page-table reclamation.
     */
    for (uint64_t i = 0; i < 3; i++) {

        if (!pmm_free_frame(
                returned_physical_addresses[i]
            )) {
            return vmm_test_failure(
                "Releasing range-unmapped physical frames"
            );
        }
    }

    vmm_test_success(
        "Three range-unmapped data frames released"
    );


    /*
     * TEST 2:
     * Repeated unmapping must report NOT_MAPPED.
     */
    uint64_t repeated_unmap_output[3] = {0};

    vmm_status_t repeated_unmap_status =
        vmm_unmap_range(
            unmap_range_virtual,
            3,
            repeated_unmap_output
        );

    if (repeated_unmap_status != VMM_STATUS_NOT_MAPPED) {

        console_write("Unexpected repeated unmap status: ");
        vmm_test_write_hex(
            (uint64_t)repeated_unmap_status
        );
        console_write("\n");

        return vmm_test_failure(
            "Repeated range unmapping returns NOT_MAPPED"
        );
    }

    vmm_test_success(
        "Repeated range unmapping returns NOT_MAPPED"
    );


    /*
     * TEST 3:
     * A range crossing a 2 MiB boundary.
     */
    uint64_t unmap_boundary_virtual =
        test_virtual_address +
        (14ULL * VMM_TEST_HUGE_PAGE_SIZE) +
        VMM_TEST_HUGE_PAGE_SIZE -
        VMM_TEST_PAGE_SIZE;

    uint64_t unmap_boundary_physical = 0;

    if (!vmm_test_alloc_contiguous_frames(
            2,
            &unmap_boundary_physical
        )) {
        return vmm_test_failure(
            "Allocating boundary-unmapping frames"
        );
    }

    if (vmm_map_range(
            unmap_boundary_virtual,
            unmap_boundary_physical,
            2,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        vmm_test_release_range(
            unmap_boundary_virtual,
            unmap_boundary_physical,
            2
        );

        return vmm_test_failure(
            "Preparing 2 MiB boundary-unmapping test"
        );
    }

    uint64_t boundary_returned_addresses[2] = {0};

    if (vmm_unmap_range(
            unmap_boundary_virtual,
            2,
            boundary_returned_addresses
        ) != VMM_STATUS_OK) {

        vmm_test_release_range(
            unmap_boundary_virtual,
            unmap_boundary_physical,
            2
        );

        return vmm_test_failure(
            "Unmapping across 2 MiB boundary"
        );
    }

    if (boundary_returned_addresses[0] !=
            unmap_boundary_physical ||
        boundary_returned_addresses[1] !=
            unmap_boundary_physical + VMM_TEST_PAGE_SIZE) {

        return vmm_test_failure(
            "Boundary unmapping returns correct physical addresses"
        );
    }

    if (vmm_query(
            unmap_boundary_virtual,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED ||
        vmm_query(
            unmap_boundary_virtual + VMM_TEST_PAGE_SIZE,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return vmm_test_failure(
            "Boundary range pages become unmapped"
        );
    }

    if (!pmm_free_frame(boundary_returned_addresses[0]) ||
        !pmm_free_frame(boundary_returned_addresses[1])) {

        return vmm_test_failure(
            "Releasing boundary-unmapped frames"
        );
    }

    vmm_test_success(
        "Two-page unmapping across 2 MiB boundary"
    );


    /*
     * TEST 4:
     * Missing mapping in the middle of a range.
     *
     * Preflight must reject the complete operation
     * without removing either of the other mappings.
     */
    uint64_t missing_middle_virtual =
        test_virtual_address +
        (16ULL * VMM_TEST_HUGE_PAGE_SIZE);

    uint64_t missing_middle_physical = 0;

    if (!vmm_test_alloc_contiguous_frames(
            3,
            &missing_middle_physical
        )) {
        return vmm_test_failure(
            "Allocating missing-middle test frames"
        );
    }

    if (vmm_map_range(
            missing_middle_virtual,
            missing_middle_physical,
            3,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        vmm_test_release_range(
            missing_middle_virtual,
            missing_middle_physical,
            3
        );

        return vmm_test_failure(
            "Preparing missing-middle test"
        );
    }

    uint64_t individually_unmapped_physical = 0;

    if (vmm_unmap_page(
            missing_middle_virtual + VMM_TEST_PAGE_SIZE,
            &individually_unmapped_physical
        ) != VMM_STATUS_OK ||
        individually_unmapped_physical !=
            missing_middle_physical + VMM_TEST_PAGE_SIZE) {

        vmm_test_release_range(
            missing_middle_virtual,
            missing_middle_physical,
            3
        );

        return vmm_test_failure(
            "Creating missing middle mapping"
        );
    }

    uint64_t missing_middle_output[3] = {0};

    vmm_status_t missing_middle_status =
        vmm_unmap_range(
            missing_middle_virtual,
            3,
            missing_middle_output
        );

    if (missing_middle_status != VMM_STATUS_NOT_MAPPED) {

        vmm_test_release_range(
            missing_middle_virtual,
            missing_middle_physical,
            3
        );

        return vmm_test_failure(
            "Missing middle mapping rejects range unmapping"
        );
    }

    vmm_test_success(
        "Missing middle mapping rejects range unmapping"
    );

    /*
     * The first and third pages must remain mapped.
     */
    bool surrounding_pages_preserved = true;

    if (vmm_query(
            missing_middle_virtual,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address != missing_middle_physical) {

        surrounding_pages_preserved = false;
    }

    if (vmm_query(
            missing_middle_virtual + 2 * VMM_TEST_PAGE_SIZE,
            &mapping
        ) != VMM_STATUS_OK ||
        mapping.physical_address !=
            missing_middle_physical + 2 * VMM_TEST_PAGE_SIZE) {

        surrounding_pages_preserved = false;
    }

    /*
     * The middle page must still be unmapped.
     */
    if (vmm_query(
            missing_middle_virtual + VMM_TEST_PAGE_SIZE,
            &mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        surrounding_pages_preserved = false;
    }

    if (!surrounding_pages_preserved) {

        vmm_test_release_range(
            missing_middle_virtual,
            missing_middle_physical,
            3
        );

        return vmm_test_failure(
            "Failed range unmapping preserves other mappings"
        );
    }

    vmm_test_success(
        "Failed range unmapping preserves other mappings"
    );

    /*
     * Clean up the remaining mappings and all three
     * caller-owned physical frames.
     */
    if (!vmm_test_release_range(
            missing_middle_virtual,
            missing_middle_physical,
            3
        )) {
        return vmm_test_failure(
            "Cleaning up missing-middle test"
        );
    }

    vmm_test_success(
        "Missing-middle test cleanup"
    );


    /*
     * TEST 5:
     * Invalid arguments and address validation.
     */
    uint64_t invalid_unmap_output[3] = {0};

    struct {
        const char *name;
        uint64_t virtual_address;
        uint64_t page_count;
        uint64_t *output;
        vmm_status_t expected_status;
    } invalid_unmap_tests[] = {

        {
            "Zero page count",
            test_virtual_address,
            0,
            invalid_unmap_output,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "NULL physical address array",
            test_virtual_address,
            1,
            0,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Unaligned virtual address",
            test_virtual_address + 1,
            1,
            invalid_unmap_output,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Noncanonical virtual address",
            0x0000800000000000ULL,
            1,
            invalid_unmap_output,
            VMM_STATUS_NON_CANONICAL
        },

        {
            "Virtual range arithmetic overflow",
            0xFFFFFFFFFFFFF000ULL,
            2,
            invalid_unmap_output,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Page-count multiplication overflow",
            test_virtual_address,
            (UINT64_MAX / VMM_TEST_PAGE_SIZE) + 2,
            invalid_unmap_output,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Canonical address gap crossing",
            0x00007FFFFFFFF000ULL,
            ((0xFFFF800000000000ULL -
              0x00007FFFFFFFF000ULL) /
             VMM_TEST_PAGE_SIZE) + 1,
            invalid_unmap_output,
            VMM_STATUS_NON_CANONICAL
        }
    };

    bool invalid_unmap_tests_passed = true;

    for (uint64_t i = 0;
         i < sizeof(invalid_unmap_tests) /
             sizeof(invalid_unmap_tests[0]);
         i++) {

        vmm_status_t actual_status =
            vmm_unmap_range(
                invalid_unmap_tests[i].virtual_address,
                invalid_unmap_tests[i].page_count,
                invalid_unmap_tests[i].output
            );

        if (actual_status !=
            invalid_unmap_tests[i].expected_status) {

            console_write("Failed invalid unmap test: ");
            console_write(invalid_unmap_tests[i].name);

            console_write("\nExpected: ");
            vmm_test_write_hex(
                (uint64_t)invalid_unmap_tests[i].expected_status
            );

            console_write("\nActual: ");
            vmm_test_write_hex((uint64_t)actual_status);

            console_write("\n");

            invalid_unmap_tests_passed = false;
            break;
        }
    }

    if (!invalid_unmap_tests_passed) {
        return vmm_test_failure(
            "Range unmapping argument validation"
        );
    }

    vmm_test_success(
        "Range unmapping argument and overflow validation"
    );


    /*
     * TEST 6:
     * A missing mapping at the beginning of a range
     * must also be rejected without modifying anything.
     */
    uint64_t missing_start_output[2] = {0};

    if (vmm_unmap_range(
            test_virtual_address +
                (20ULL * VMM_TEST_HUGE_PAGE_SIZE),
            2,
            missing_start_output
        ) != VMM_STATUS_NOT_MAPPED) {

        return vmm_test_failure(
            "Missing first mapping rejection"
        );
    }

    vmm_test_success(
        "Missing first mapping rejection"
    );


    /*
     * TEST 7:
     * Verify the complete range-unmapping test suite.
     */
    vmm_test_success(
        "VMM range-unmapping comprehensive tests"
    );


    /*
     * STEP 14: PAGE PROTECTION TESTS
     */

    console_write(
        "\n[ TEST ] VMM page-protection management...\n"
    );

    uint64_t protection_virtual =
        test_virtual_address +
        (24ULL * VMM_TEST_HUGE_PAGE_SIZE);

    uint64_t protection_physical = 0;

    uint64_t protection_frames_before =
        pmm_get_free_frames();

    /*
     * Allocate three contiguous frames.
     */
    if (!vmm_test_alloc_contiguous_frames(
            3,
            &protection_physical
        )) {

        return vmm_test_failure(
            "Protection test physical allocation"
        );
    }

    /*
     * Ensure the test range is initially unmapped.
     */
    bool protection_range_clear = true;

    for (uint64_t i = 0; i < 3; i++) {

        vmm_mapping_info_t current_mapping;

        if (vmm_query(
                protection_virtual +
                    (i * VMM_TEST_PAGE_SIZE),
                &current_mapping
            ) != VMM_STATUS_NOT_MAPPED) {

            protection_range_clear = false;
            break;
        }
    }

    if (!protection_range_clear) {

        for (uint64_t i = 0; i < 3; i++) {
            pmm_free_frame(
                protection_physical +
                (i * VMM_TEST_PAGE_SIZE)
            );
        }

        return vmm_test_failure(
            "Protection test virtual range availability"
        );
    }

    /*
     * Create three writable, supervisor-only,
     * non-executable mappings.
     */
    if (vmm_map_range(
            protection_virtual,
            protection_physical,
            3,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        for (uint64_t i = 0; i < 3; i++) {
            pmm_free_frame(
                protection_physical +
                (i * VMM_TEST_PAGE_SIZE)
            );
        }

        return vmm_test_failure(
            "Protection test initial mapping"
        );
    }

    vmm_test_success(
        "Three-page protection test mapping"
    );


    /*
     * TEST 1:
     * Change all three pages to read-only,
     * supervisor-only and non-executable.
     */
    if (vmm_protect_range(
            protection_virtual,
            3,
            VMM_MAP_NONE
        ) != VMM_STATUS_OK) {

        return vmm_test_failure(
            "Read-only range protection"
        );
    }

    bool readonly_passed = true;

    for (uint64_t i = 0; i < 3; i++) {

        vmm_mapping_info_t current_mapping;

        if (vmm_query(
                protection_virtual +
                    (i * VMM_TEST_PAGE_SIZE),
                &current_mapping
            ) != VMM_STATUS_OK) {

            readonly_passed = false;
            break;
        }

        if (current_mapping.writable ||
            current_mapping.user_accessible ||
            current_mapping.executable ||
            current_mapping.page_size != VMM_TEST_PAGE_SIZE ||
            current_mapping.physical_address !=
                protection_physical +
                (i * VMM_TEST_PAGE_SIZE)) {

            readonly_passed = false;
            break;
        }
    }

    if (!readonly_passed) {
        return vmm_test_failure(
            "Read-only range permission verification"
        );
    }

    vmm_test_success(
        "Three-page read-only protection"
    );

    vmm_test_success(
        "Read-only range preserves physical mappings"
    );


    /*
     * TEST 2:
     * Change only the middle page to writable.
     */
    if (vmm_protect_page(
            protection_virtual + VMM_TEST_PAGE_SIZE,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        return vmm_test_failure(
            "Single-page writable protection"
        );
    }

    vmm_mapping_info_t protection_mapping;

    if (vmm_query(
            protection_virtual + VMM_TEST_PAGE_SIZE,
            &protection_mapping
        ) != VMM_STATUS_OK ||
        !protection_mapping.writable ||
        protection_mapping.executable ||
        protection_mapping.user_accessible) {

        return vmm_test_failure(
            "Single-page permission verification"
        );
    }

    vmm_test_success(
        "Single-page writable protection"
    );


    /*
     * TEST 3:
     * Make the middle page executable.
     *
     * It must first be read-only to satisfy W^X.
     */
    if (vmm_protect_page(
            protection_virtual + VMM_TEST_PAGE_SIZE,
            VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_OK) {

        return vmm_test_failure(
            "Single-page executable protection"
        );
    }

    if (vmm_query(
            protection_virtual + VMM_TEST_PAGE_SIZE,
            &protection_mapping
        ) != VMM_STATUS_OK ||
        protection_mapping.writable ||
        !protection_mapping.executable) {

        return vmm_test_failure(
            "Executable permission verification"
        );
    }

    vmm_test_success(
        "Read-only executable protection"
    );


    /*
     * TEST 4:
     * Reject writable + executable protection.
     */
    if (vmm_protect_page(
            protection_virtual + VMM_TEST_PAGE_SIZE,
            VMM_MAP_WRITABLE | VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Protection W^X rejection"
        );
    }

    vmm_test_success(
        "Protection W^X enforcement"
    );


    /*
     * TEST 5:
     * Ensure the older executable API also enforces W^X.
     *
     * First make the first page writable again.
     */
    if (vmm_protect_page(
            protection_virtual,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        return vmm_test_failure(
            "Legacy W^X test writable setup"
        );
    }

    if (vmm_set_page_executable(
            protection_virtual,
            true
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Legacy executable API W^X enforcement"
        );
    }

    vmm_test_success(
        "Legacy executable API W^X enforcement"
    );


    /*
     * TEST 6:
     * Reject a range containing an unmapped middle page.
     *
     * The existing first and third pages must remain unchanged.
     */
    uint64_t missing_protection_physical = 0;

    if (!vmm_test_alloc_contiguous_frames(
            3,
            &missing_protection_physical
        )) {

        return vmm_test_failure(
            "Missing-page protection allocation"
        );
    }

    uint64_t missing_protection_virtual =
        protection_virtual +
        (8ULL * VMM_TEST_HUGE_PAGE_SIZE);

    if (vmm_map_range(
            missing_protection_virtual,
            missing_protection_physical,
            3,
            VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        for (uint64_t i = 0; i < 3; i++) {
            pmm_free_frame(
                missing_protection_physical +
                (i * VMM_TEST_PAGE_SIZE)
            );
        }

        return vmm_test_failure(
            "Missing-page protection setup"
        );
    }

    uint64_t removed_middle_physical = 0;

    if (vmm_unmap_page(
            missing_protection_virtual +
                VMM_TEST_PAGE_SIZE,
            &removed_middle_physical
        ) != VMM_STATUS_OK) {

        return vmm_test_failure(
            "Missing-page protection setup unmap"
        );
    }

    if (removed_middle_physical !=
        missing_protection_physical +
        VMM_TEST_PAGE_SIZE) {

        return vmm_test_failure(
            "Missing-page protection physical verification"
        );
    }

    /*
     * Reject the complete range before changing the first page.
     */
    if (vmm_protect_range(
            missing_protection_virtual,
            3,
            VMM_MAP_NONE
        ) != VMM_STATUS_NOT_MAPPED) {

        return vmm_test_failure(
            "Missing-page protection rejection"
        );
    }

    vmm_mapping_info_t first_missing_mapping;
    vmm_mapping_info_t third_missing_mapping;

    if (vmm_query(
            missing_protection_virtual,
            &first_missing_mapping
        ) != VMM_STATUS_OK ||
        !first_missing_mapping.writable ||
        first_missing_mapping.executable) {

        return vmm_test_failure(
            "Protection preflight preserves first page"
        );
    }

    if (vmm_query(
            missing_protection_virtual +
                (2 * VMM_TEST_PAGE_SIZE),
            &third_missing_mapping
        ) != VMM_STATUS_OK ||
        !third_missing_mapping.writable ||
        third_missing_mapping.executable) {

        return vmm_test_failure(
            "Protection preflight preserves third page"
        );
    }

    vmm_test_success(
        "Missing-page protection rejection"
    );

    vmm_test_success(
        "Protection preflight preserves existing permissions"
    );

    /*
     * Clean up the missing-page test.
     *
     * The first and third pages are still mapped.
     * The middle page was already unmapped.
     *
     * vmm_test_release_range() removes remaining mappings
     * and releases all three caller-owned physical frames.
     */
    if (!vmm_test_release_range(
            missing_protection_virtual,
            missing_protection_physical,
            3
        )) {

        return vmm_test_failure(
            "Missing-page protection cleanup"
        );
    }

    vmm_test_success(
        "Missing-page protection cleanup"
    );


    /*
     * TEST 7:
     * Invalid protection arguments.
     */
    struct {
        const char *name;
        uint64_t virtual_address;
        uint64_t flags;
        vmm_status_t expected;
    } invalid_protection_tests[] = {

        {
            "Unaligned virtual address",
            protection_virtual + 1,
            VMM_MAP_NONE,
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Noncanonical virtual address",
            0x0000800000000000ULL,
            VMM_MAP_NONE,
            VMM_STATUS_NON_CANONICAL
        },

        {
            "Unsupported protection flags",
            protection_virtual,
            (1ULL << 12),
            VMM_STATUS_INVALID_ARGUMENT
        },

        {
            "Writable executable protection",
            protection_virtual,
            VMM_MAP_WRITABLE | VMM_MAP_EXECUTABLE,
            VMM_STATUS_INVALID_ARGUMENT
        }
    };

    bool invalid_protection_passed = true;

    for (uint64_t i = 0;
         i < sizeof(invalid_protection_tests) /
             sizeof(invalid_protection_tests[0]);
         i++) {

        vmm_status_t actual =
            vmm_protect_page(
                invalid_protection_tests[i].virtual_address,
                invalid_protection_tests[i].flags
            );

        if (actual !=
            invalid_protection_tests[i].expected) {

            console_write(
                "Failed protection argument test: "
            );

            console_write(
                invalid_protection_tests[i].name
            );

            console_write("\nExpected: ");
            vmm_test_write_hex(
                (uint64_t)invalid_protection_tests[i].expected
            );

            console_write("\nActual: ");
            vmm_test_write_hex((uint64_t)actual);

            console_write("\n");

            invalid_protection_passed = false;
            break;
        }
    }

    if (!invalid_protection_passed) {
        return vmm_test_failure(
            "Protection argument validation"
        );
    }

    vmm_test_success(
        "Protection argument validation"
    );


    /*
     * TEST 8:
     * Reject a range with zero pages and a range
     * whose address arithmetic overflows.
     */
    if (vmm_protect_range(
            protection_virtual,
            0,
            VMM_MAP_NONE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Zero-page protection range rejection"
        );
    }

    if (vmm_protect_range(
            0xFFFFFFFFFFFFF000ULL,
            2,
            VMM_MAP_NONE
        ) != VMM_STATUS_INVALID_ARGUMENT) {

        return vmm_test_failure(
            "Protection range overflow rejection"
        );
    }

    vmm_test_success(
        "Protection range size and overflow validation"
    );


    /*
     * TEST 9:
     * Clean up the primary protection test.
     */
    if (!vmm_test_release_range(
            protection_virtual,
            protection_physical,
            3
        )) {

        return vmm_test_failure(
            "Protection test mapping cleanup"
        );
    }

    if (pmm_get_free_frames() !=
        protection_frames_before) {

        return vmm_test_failure(
            "Protection test PMM frame accounting"
        );
    }

    vmm_test_success(
        "Protection tests preserve PMM frame accounting"
    );

    vmm_test_success(
        "VMM page-protection comprehensive tests"
    );


    console_write("Test virtual address: ");
    vmm_test_write_hex(test_virtual_address);

    console_write("\nTest physical address: ");
    vmm_test_write_hex(test_physical_address);

    console_write("\n");

    vmm_test_success("Single-page mapping and unmapping tests");
    return true;
}
