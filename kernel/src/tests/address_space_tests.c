#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "drivers/console/console.h"
#include "mm/vmm.h"
#include "tests/address_space_tests.h"


static bool address_space_test_failure(const char *message) {

    console_set_color(0xFF5555, 0x101820);

    console_write("[ FAILED ] ");
    console_write(message);
    console_write("\n");

    console_set_color(0xFFFFFF, 0x101820);

    return false;
}


static void address_space_test_success(const char *message) {

    console_set_color(0x36D399, 0x101820);

    console_write("[ OK ] ");
    console_write(message);
    console_write("\n");

    console_set_color(0xFFFFFF, 0x101820);
}


/*
 * Test address-space root creation, kernel mapping sharing,
 * lower-half isolation and reference-counted destruction.
 */
bool address_space_run_tests(void) {

    console_write("\nRunning address-space foundation tests...\n");


    /*
     * The kernel address space must exist after vmm_init().
     */
    vmm_address_space_t *kernel =
        vmm_kernel_address_space();

    if (kernel == NULL) {
        return address_space_test_failure(
            "Kernel address-space availability"
        );
    }


    /*
     * Use this function's address as a stable kernel mapping
     * that should be visible in every address space.
     */
    uint64_t kernel_probe =
        (uint64_t)(uintptr_t)&address_space_run_tests;

    vmm_mapping_info_t kernel_mapping;

    if (vmm_query(
            kernel_probe,
            &kernel_mapping
        ) != VMM_STATUS_OK) {

        return address_space_test_failure(
            "Kernel probe mapping exists"
        );
    }

    if (kernel_mapping.user_accessible) {
        return address_space_test_failure(
            "Kernel probe is supervisor-only"
        );
    }


    /*
     * Create two independent address spaces.
     */
    vmm_address_space_t *first =
        vmm_address_space_create();

    if (first == NULL) {
        return address_space_test_failure(
            "First address-space creation"
        );
    }

    vmm_address_space_t *second =
        vmm_address_space_create();

    if (second == NULL) {

        vmm_address_space_put(first);

        return address_space_test_failure(
            "Second address-space creation"
        );
    }


    /*
     * Their PML4 roots must be independent physical frames.
     */
    uint64_t kernel_root = 0;
    uint64_t first_root = 0;
    uint64_t second_root = 0;

    bool roots_valid =
        vmm_address_space_get_root_physical(
            kernel,
            &kernel_root
        ) &&
        vmm_address_space_get_root_physical(
            first,
            &first_root
        ) &&
        vmm_address_space_get_root_physical(
            second,
            &second_root
        );

    if (!roots_valid ||
        first_root == 0 ||
        second_root == 0 ||
        first_root == kernel_root ||
        second_root == kernel_root ||
        first_root == second_root ||
        (first_root & 0xFFFULL) != 0 ||
        (second_root & 0xFFFULL) != 0) {

        vmm_address_space_put(second);
        vmm_address_space_put(first);

        return address_space_test_failure(
            "Independent aligned PML4 roots"
        );
    }

    address_space_test_success(
        "Independent PML4 roots created"
    );


    /*
     * The kernel mapping must be visible through both roots.
     */
    vmm_mapping_info_t first_mapping;
    vmm_mapping_info_t second_mapping;

    if (vmm_address_space_query(
            first,
            kernel_probe,
            &first_mapping
        ) != VMM_STATUS_OK ||
        vmm_address_space_query(
            second,
            kernel_probe,
            &second_mapping
        ) != VMM_STATUS_OK) {

        vmm_address_space_put(second);
        vmm_address_space_put(first);

        return address_space_test_failure(
            "Shared kernel mapping visibility"
        );
    }

    if (first_mapping.physical_address !=
            kernel_mapping.physical_address ||
        second_mapping.physical_address !=
            kernel_mapping.physical_address ||
        first_mapping.user_accessible ||
        second_mapping.user_accessible ||
        !first_mapping.executable ||
        !second_mapping.executable) {

        vmm_address_space_put(second);
        vmm_address_space_put(first);

        return address_space_test_failure(
            "Shared kernel mapping permissions"
        );
    }

    address_space_test_success(
        "Kernel mappings shared as supervisor-only"
    );


    /*
     * A newly created address space must not inherit
     * lower-half mappings.
     */
    vmm_mapping_info_t user_mapping;

    if (vmm_address_space_query(
            first,
            0x0000400000000000ULL,
            &user_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        vmm_address_space_put(second);
        vmm_address_space_put(first);

        return address_space_test_failure(
            "New address space has an empty user half"
        );
    }

    address_space_test_success(
        "New address space starts with an empty user half"
    );


    /*
     * Check reference counting.
     */
    if (!vmm_address_space_get(first) ||
        !vmm_address_space_put(first)) {

        vmm_address_space_put(second);
        vmm_address_space_put(first);

        return address_space_test_failure(
            "Address-space reference acquisition and release"
        );
    }

    /*
     * Releasing the extra reference must not destroy the
     * address space.
     */
    if (vmm_address_space_query(
            first,
            kernel_probe,
            &first_mapping
        ) != VMM_STATUS_OK) {

        vmm_address_space_put(second);
        vmm_address_space_put(first);

        return address_space_test_failure(
            "Address space remains alive while referenced"
        );
    }

    address_space_test_success(
        "Address-space reference counting"
    );


    /*
     * The kernel address space is permanent.
     */
    if (vmm_address_space_put(kernel)) {

        vmm_address_space_put(second);
        vmm_address_space_put(first);

        return address_space_test_failure(
            "Rejecting destruction of kernel address space"
        );
    }


    /*
     * Destroy both independent address spaces.
     */
    if (!vmm_address_space_put(second) ||
        !vmm_address_space_put(first)) {

        return address_space_test_failure(
            "Address-space root destruction"
        );
    }

    address_space_test_success(
        "Independent address-space roots destroyed"
    );

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] All address-space foundation tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    return true;
}
