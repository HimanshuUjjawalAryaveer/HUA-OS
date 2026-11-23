#include "tests/user_memory_tests.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/console/console.h"

#include "mm/pmm.h"
#include "mm/user_memory.h"
#include "mm/vmm.h"


/*
 * Dedicated temporary test range.
 *
 * The test first confirms that both pages are unmapped.
 * It never silently replaces existing mappings.
 */
#define USER_MEMORY_TEST_VA 0x0000004000000000ULL


/*
 * Print one test result and compare the returned status.
 */
static bool user_memory_expect_status(
    const char *name,
    user_memory_status_t actual,
    user_memory_status_t expected
) {

    if (actual != expected) {

        console_write("[ FAILED ] ");
        console_write(name);
        console_write("\n");

        return false;
    }

    console_write("[ OK ] ");
    console_write(name);
    console_write("\n");

    return true;
}


/*
 * Compare two byte arrays without depending on hosted libc.
 */
static bool user_memory_test_bytes_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t length
) {

    if (left == NULL || right == NULL) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {

        if (left[i] != right[i]) {
            return false;
        }
    }

    return true;
}


/*
 * Ensure a test address is currently unmapped.
 */
static bool user_memory_test_is_unmapped(
    vmm_address_space_t *space,
    uint64_t address
) {

    vmm_mapping_info_t mapping = {0};

    return
        vmm_address_space_query(
            space,
            address,
            &mapping
        ) == VMM_STATUS_NOT_MAPPED;
}


/*
 * Release one temporary test mapping and its PMM frame.
 *
 * Never free a frame if its mapping could not be removed.
 */
static bool user_memory_test_release_page(
    vmm_address_space_t *space,
    uint64_t virtual_address,
    uint64_t *physical_address,
    bool *mapped
) {

    if (space == NULL ||
        physical_address == NULL ||
        mapped == NULL) {

        return false;
    }

    if (*mapped) {

        uint64_t unmapped_physical = 0;

        if (vmm_address_space_unmap_page(
                space,
                virtual_address,
                &unmapped_physical
            ) != VMM_STATUS_OK) {

            /*
             * The mapping might still exist.
             * Do not free its physical frame.
             */
            return false;
        }

        *mapped = false;

        if (unmapped_physical != *physical_address) {

            /*
             * Unexpected physical ownership.
             * Fail closed instead of freeing an arbitrary frame.
             */
            return false;
        }
    }

    if (*physical_address != 0) {

        if (!pmm_free_frame(*physical_address)) {
            return false;
        }

        *physical_address = 0;
    }

    return true;
}


/*
 * Test copying across two adjacent user pages.
 *
 * This exercises:
 * - Translation of two independent pages.
 * - A copy beginning near the end of the first page.
 * - Continuation at offset zero of the second page.
 * - The same behavior in both copy directions.
 */
static bool user_memory_test_cross_page(
    vmm_address_space_t *space
) {

    console_write(
        "\n[ USER MEMORY ] Cross-page copy tests\n"
    );

    uint64_t first_physical = 0;

    uint64_t second_physical = 0;

    bool first_mapped = false;

    bool second_mapped = false;

    bool success = false;

    bool first_release_ok = false;
    bool second_release_ok = false;

    /*
     * Refuse to use an address that is already occupied.
     */
    if (!user_memory_test_is_unmapped(
            space,
            USER_MEMORY_TEST_VA
        ) ||
        !user_memory_test_is_unmapped(
            space,
            USER_MEMORY_TEST_VA + PMM_PAGE_SIZE
        )) {

        console_write(
            "[ FAILED ] Cross-page test VA is not free\n"
        );

        return false;
    }

    /*
     * Allocate two independent physical frames.
     */
    first_physical = pmm_alloc_frame();

    if (first_physical == 0) {
        goto cleanup;
    }

    second_physical = pmm_alloc_frame();

    if (second_physical == 0) {
        goto cleanup;
    }

    /*
     * Map both pages into the test process.
     *
     * User RW and non-executable.
     */
    if (vmm_address_space_map_page(
            space,
            USER_MEMORY_TEST_VA,
            first_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        goto cleanup;
    }

    first_mapped = true;

    if (vmm_address_space_map_page(
            space,
            USER_MEMORY_TEST_VA + PMM_PAGE_SIZE,
            second_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        goto cleanup;
    }

    second_mapped = true;

    /*
     * Fill a test buffer with a nontrivial byte pattern.
     */
    uint8_t source[48];

    uint8_t destination[48];

    for (size_t i = 0; i < sizeof(source); i++) {

        source[i] =
            (uint8_t)((i * 37U) + 11U);

        destination[i] = 0;
    }

    /*
     * Begin 24 bytes before the boundary.
     * The 48-byte copy crosses into the second page.
     */
    uint64_t cross_page_address =
        USER_MEMORY_TEST_VA +
        PMM_PAGE_SIZE - 24ULL;

    if (!user_memory_expect_status(
            "Cross-page copy_to_user",
            copy_to_user(
                space,
                cross_page_address,
                source,
                sizeof(source)
            ),
            USER_MEMORY_STATUS_OK
        )) {

        goto cleanup;
    }

    if (!user_memory_expect_status(
            "Cross-page copy_from_user",
            copy_from_user(
                space,
                destination,
                cross_page_address,
                sizeof(destination)
            ),
            USER_MEMORY_STATUS_OK
        )) {

        goto cleanup;
    }

    if (!user_memory_test_bytes_equal(
            source,
            destination,
            sizeof(source)
        )) {

        console_write(
            "[ FAILED ] Cross-page data integrity\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Cross-page data integrity\n"
    );

    success = true;


cleanup:

    /*
     * Cleanup happens only after the test has stopped
     * accessing the temporary pages.
     */
    first_release_ok =
        user_memory_test_release_page(
            space,
            USER_MEMORY_TEST_VA,
            &first_physical,
            &first_mapped
        );

    second_release_ok =
        user_memory_test_release_page(
            space,
            USER_MEMORY_TEST_VA + PMM_PAGE_SIZE,
            &second_physical,
            &second_mapped
        );

    if (!first_release_ok ||
        !second_release_ok) {

        console_write(
            "[ FAILED ] Cross-page test cleanup\n"
        );

        return false;
    }

    console_write(
        "[ OK ] Cross-page test mappings and frames released\n"
    );

    return success;
}


/*
 * Main user-memory test suite.
 */
bool user_memory_run_tests(
    vmm_address_space_t *space,
    uint64_t code_user_address,
    uint64_t code_physical_address,
    uint64_t stack_user_base,
    uint64_t stack_user_top,
    uint64_t stack_physical_address
) {

    console_write(
        "\n========== USER MEMORY ACCESS TESTS ==========\n"
    );

    if (space == NULL ||
        code_user_address == 0 ||
        code_physical_address == 0 ||
        stack_user_base == 0 ||
        stack_user_top <= stack_user_base ||
        stack_physical_address == 0) {

        console_write(
            "[ FAILED ] User-memory test prerequisites\n"
        );

        return false;
    }

    if (stack_user_top - stack_user_base !=
        PMM_PAGE_SIZE) {

        console_write(
            "[ FAILED ] Expected one-page test stack\n"
        );

        return false;
    }

    /*
     * Obtain trusted HHDM references to the known PMM frames.
     * These are expected test backing pages, not user pointers.
     */
    uint8_t *code_frame =
        (uint8_t *)pmm_frame_to_virt(
            code_physical_address
        );

    uint8_t *stack_frame =
        (uint8_t *)pmm_frame_to_virt(
            stack_physical_address
        );

    if (code_frame == NULL ||
        stack_frame == NULL) {

        console_write(
            "[ FAILED ] User-memory test backing frames\n"
        );

        return false;
    }


    /* ------------------------------------------------------------------ */
    /* Basic range validation                                             */
    /* ------------------------------------------------------------------ */

    if (!user_memory_expect_status(
            "Readable user code range",
            user_range_validate(
                space,
                code_user_address,
                32,
                USER_MEMORY_ACCESS_READ
            ),
            USER_MEMORY_STATUS_OK
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Writable user stack range",
            user_range_validate(
                space,
                stack_user_base,
                32,
                USER_MEMORY_ACCESS_READ |
                USER_MEMORY_ACCESS_WRITE
            ),
            USER_MEMORY_STATUS_OK
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Reject write access to RX code",
            user_range_validate(
                space,
                code_user_address,
                1,
                USER_MEMORY_ACCESS_WRITE
            ),
            USER_MEMORY_STATUS_NOT_WRITABLE
        )) {

        return false;
    }


    /* ------------------------------------------------------------------ */
    /* Zero-length and invalid-argument tests                              */
    /* ------------------------------------------------------------------ */

    if (!user_memory_expect_status(
            "Zero-length copy is a no-op",
            copy_from_user(
                space,
                NULL,
                UINT64_MAX,
                0
            ),
            USER_MEMORY_STATUS_OK
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Reject zero access mask",
            user_range_validate(
                space,
                code_user_address,
                1,
                USER_MEMORY_ACCESS_NONE
            ),
            USER_MEMORY_STATUS_INVALID_ARGUMENT
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Reject NULL kernel destination",
            copy_from_user(
                space,
                NULL,
                stack_user_base,
                1
            ),
            USER_MEMORY_STATUS_INVALID_ARGUMENT
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Reject NULL kernel source",
            copy_to_user(
                space,
                stack_user_base,
                NULL,
                1
            ),
            USER_MEMORY_STATUS_INVALID_ARGUMENT
        )) {

        return false;
    }


    /* ------------------------------------------------------------------ */
    /* Arithmetic and user-boundary rejection                              */
    /* ------------------------------------------------------------------ */

    if (!user_memory_expect_status(
            "Reject user range arithmetic overflow",
            user_range_validate(
                space,
                UINT64_MAX - 3ULL,
                8,
                USER_MEMORY_ACCESS_READ
            ),
            USER_MEMORY_STATUS_RANGE_OVERFLOW
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Reject range larger than copy limit",
            user_range_validate(
                space,
                stack_user_base,
                USER_MEMORY_MAX_COPY_BYTES + 1ULL,
                USER_MEMORY_ACCESS_READ
            ),
            USER_MEMORY_STATUS_TOO_LARGE
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Reject first upper-half address",
            user_range_validate(
                space,
                0x0000800000000000ULL,
                1,
                USER_MEMORY_ACCESS_READ
            ),
            USER_MEMORY_STATUS_NOT_USER_RANGE
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Reject kernel HHDM address",
            user_range_validate(
                space,
                0xFFFF800000000000ULL,
                1,
                USER_MEMORY_ACCESS_READ
            ),
            USER_MEMORY_STATUS_NOT_USER_RANGE
        )) {

        return false;
    }


    /* ------------------------------------------------------------------ */
    /* Unmapped user address                                               */
    /* ------------------------------------------------------------------ */

    if (!user_memory_expect_status(
            "Reject unmapped address above user stack",
            user_range_validate(
                space,
                stack_user_top,
                1,
                USER_MEMORY_ACCESS_READ
            ),
            USER_MEMORY_STATUS_NOT_MAPPED
        )) {

        return false;
    }


    /* ------------------------------------------------------------------ */
    /* Copy user stack data to and from the kernel                         */
    /* ------------------------------------------------------------------ */

    uint8_t stack_source[64];

    uint8_t stack_destination[64];

    for (size_t i = 0; i < sizeof(stack_source); i++) {

        stack_source[i] =
            (uint8_t)(0xA5U ^ (uint8_t)(i * 13U));

        stack_destination[i] = 0;
    }

    uint64_t stack_test_address =
        stack_user_base + 128ULL;

    if (!user_memory_expect_status(
            "Copy kernel data into user stack",
            copy_to_user(
                space,
                stack_test_address,
                stack_source,
                sizeof(stack_source)
            ),
            USER_MEMORY_STATUS_OK
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Copy user stack data into kernel",
            copy_from_user(
                space,
                stack_destination,
                stack_test_address,
                sizeof(stack_destination)
            ),
            USER_MEMORY_STATUS_OK
        )) {

        return false;
    }

    if (!user_memory_test_bytes_equal(
            stack_source,
            stack_destination,
            sizeof(stack_source)
        )) {

        console_write(
            "[ FAILED ] User stack round-trip integrity\n"
        );

        return false;
    }

    console_write(
        "[ OK ] User stack round-trip integrity\n"
    );


    /* ------------------------------------------------------------------ */
    /* Read from the executable, read-only code page                       */
    /* ------------------------------------------------------------------ */

    uint8_t code_copy[32];

    for (size_t i = 0; i < sizeof(code_copy); i++) {
        code_copy[i] = 0;
    }

    if (!user_memory_expect_status(
            "Read user RX code page",
            copy_from_user(
                space,
                code_copy,
                code_user_address,
                sizeof(code_copy)
            ),
            USER_MEMORY_STATUS_OK
        )) {

        return false;
    }

    if (!user_memory_test_bytes_equal(
            code_copy,
            code_frame,
            sizeof(code_copy)
        )) {

        console_write(
            "[ FAILED ] User code read integrity\n"
        );

        return false;
    }

    console_write(
        "[ OK ] User code read integrity\n"
    );


    /* ------------------------------------------------------------------ */
    /* Reject writes into the RX code mapping                              */
    /* ------------------------------------------------------------------ */

    uint8_t forbidden_write[8] = {
        0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88
    };

    if (!user_memory_expect_status(
            "Reject copy_to_user into RX code",
            copy_to_user(
                space,
                code_user_address,
                forbidden_write,
                sizeof(forbidden_write)
            ),
            USER_MEMORY_STATUS_NOT_WRITABLE
        )) {

        return false;
    }

    if (!user_memory_test_bytes_equal(
            code_copy,
            code_frame,
            sizeof(code_copy)
        )) {

        console_write(
            "[ FAILED ] Read-only user code was modified\n"
        );

        return false;
    }

    console_write(
        "[ OK ] Read-only code remained unchanged\n"
    );


    /* ------------------------------------------------------------------ */
    /* Verify no partial copy when the range crosses into an unmapped page */
    /* ------------------------------------------------------------------ */

    uint8_t boundary_marker[8] = {
        0xD1, 0xD2, 0xD3, 0xD4,
        0xD5, 0xD6, 0xD7, 0xD8
    };

    uint8_t boundary_attempt[16] = {
        0xA1, 0xA2, 0xA3, 0xA4,
        0xA5, 0xA6, 0xA7, 0xA8,
        0xB1, 0xB2, 0xB3, 0xB4,
        0xB5, 0xB6, 0xB7, 0xB8
    };

    uint8_t boundary_readback[8] = {0};

    uint64_t boundary_address =
        stack_user_top - sizeof(boundary_marker);

    /*
     * Put a known marker in the final eight bytes of the
     * mapped stack page.
     */
    if (copy_to_user(
            space,
            boundary_address,
            boundary_marker,
            sizeof(boundary_marker)
        ) != USER_MEMORY_STATUS_OK) {

        console_write(
            "[ FAILED ] Boundary marker setup\n"
        );

        return false;
    }

    /*
     * The requested 16-byte range includes eight bytes from
     * the mapped stack and eight bytes from the unmapped page.
     */
    if (!user_memory_expect_status(
            "Reject copy_from_user crossing unmapped page",
            copy_from_user(
                space,
                stack_destination,
                boundary_address,
                sizeof(boundary_attempt)
            ),
            USER_MEMORY_STATUS_NOT_MAPPED
        )) {

        return false;
    }

    /*
     * The kernel destination must remain unchanged because
     * the copy is rejected during preflight.
     */
    for (size_t i = 0;
         i < sizeof(boundary_attempt);
         i++) {

        stack_destination[i] = 0xCC;
    }

    if (copy_from_user(
            space,
            stack_destination,
            boundary_address,
            sizeof(boundary_attempt)
        ) != USER_MEMORY_STATUS_NOT_MAPPED) {

        console_write(
            "[ FAILED ] Unmapped source unexpectedly copied\n"
        );

        return false;
    }

    for (size_t i = 0;
         i < sizeof(boundary_attempt);
         i++) {

        if (stack_destination[i] != 0xCC) {

            console_write(
                "[ FAILED ] Partial kernel destination modification\n"
            );

            return false;
        }
    }

    console_write(
        "[ OK ] Invalid source caused no partial kernel copy\n"
    );


    /* ------------------------------------------------------------------ */
    /* Verify copy_to_user also performs complete preflight                */
    /* ------------------------------------------------------------------ */

    if (!user_memory_expect_status(
            "Reject copy_to_user crossing unmapped page",
            copy_to_user(
                space,
                boundary_address,
                boundary_attempt,
                sizeof(boundary_attempt)
            ),
            USER_MEMORY_STATUS_NOT_MAPPED
        )) {

        return false;
    }

    if (!user_memory_expect_status(
            "Read back boundary marker",
            copy_from_user(
                space,
                boundary_readback,
                boundary_address,
                sizeof(boundary_readback)
            ),
            USER_MEMORY_STATUS_OK
        )) {

        return false;
    }

    if (!user_memory_test_bytes_equal(
            boundary_marker,
            boundary_readback,
            sizeof(boundary_marker)
        )) {

        console_write(
            "[ FAILED ] Partial user destination modification\n"
        );

        return false;
    }

    console_write(
        "[ OK ] Invalid destination caused no partial user copy\n"
    );


    /* ------------------------------------------------------------------ */
    /* Two-page boundary-crossing success tests                            */
    /* ------------------------------------------------------------------ */

    if (!user_memory_test_cross_page(space)) {
        return false;
    }


    /* ------------------------------------------------------------------ */
    /* Final success                                                       */
    /* ------------------------------------------------------------------ */

    console_write(
        "[ OK ] User range bounds and overflow checks\n"
    );

    console_write(
        "[ OK ] User page permission enforcement\n"
    );

    console_write(
        "[ OK ] User/kernel data-copy integrity\n"
    );

    console_write(
        "[ OK ] All-or-nothing copy preflight\n"
    );

    console_write(
        "========== USER MEMORY ACCESS TESTS PASSED ==========\n"
    );

    return true;
}
