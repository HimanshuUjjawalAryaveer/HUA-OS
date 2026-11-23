#include "tests/gdt_tests.h"

#include "arch/x86_64/gdt.h"
#include "drivers/console/console.h"

#include <stdint.h>
#include <stddef.h>


#define GDT_TEST_CHECK(condition, name) \
    do { \
        if (!(condition)) { \
            console_write("[ FAILED ] GDT: " name "\n"); \
            return false; \
        } \
    } while (0)


/*
 * Validate a canonical upper-half address.
 *
 * HUA currently uses four-level x86-64 paging.
 */
static bool gdt_test_is_upper_canonical(
    uintptr_t address
) {

    return ((uint64_t)address >> 47) == 0x1FFFFULL;
}


/*
 * Extract the access byte from a GDT descriptor.
 */
static uint8_t gdt_test_access_byte(
    uint64_t descriptor
) {

    return (uint8_t)(descriptor >> 40);
}


/*
 * Extract the descriptor flags nibble.
 */
static uint8_t gdt_test_flags(
    uint64_t descriptor
) {

    return (uint8_t)((descriptor >> 52) & 0x0F);
}

/*
 * Verify GDT layout, descriptor privilege levels,
 * descriptor types and long-mode configuration.
 */
static bool gdt_test_descriptors(void) {

    uint64_t null_descriptor = UINT64_MAX;
    uint64_t kernel_code = 0;
    uint64_t kernel_data = 0;
    uint64_t tss_low = 0;
    uint64_t tss_high = 0;
    uint64_t user_data = 0;
    uint64_t user_code = 0;

    GDT_TEST_CHECK(
        gdt_test_get_descriptor(0, &null_descriptor) &&
        null_descriptor == 0,
        "null descriptor"
    );

    GDT_TEST_CHECK(
        gdt_test_get_descriptor(1, &kernel_code),
        "kernel code descriptor readable"
    );

    GDT_TEST_CHECK(
        gdt_test_get_descriptor(2, &kernel_data),
        "kernel data descriptor readable"
    );

    GDT_TEST_CHECK(
        gdt_test_get_descriptor(3, &tss_low),
        "TSS lower descriptor readable"
    );

    GDT_TEST_CHECK(
        gdt_test_get_descriptor(4, &tss_high),
        "TSS upper descriptor readable"
    );

    GDT_TEST_CHECK(
        gdt_test_get_descriptor(
            GDT_USER_DATA_INDEX,
            &user_data
        ),
        "user data descriptor readable"
    );

    GDT_TEST_CHECK(
        gdt_test_get_descriptor(
            GDT_USER_CODE_INDEX,
            &user_code
        ),
        "user code descriptor readable"
    );

    /*
     * Kernel code:
     * P=1, DPL=0, S=1, executable, readable.
     */
    GDT_TEST_CHECK(
        gdt_test_access_byte(kernel_code) == 0x9B,
        "kernel code access attributes"
    );

    GDT_TEST_CHECK(
        gdt_test_flags(kernel_code) == 0x0A,
        "kernel code long-mode flags"
    );

    /*
     * Kernel data:
     * P=1, DPL=0, S=1, writable.
     */
    GDT_TEST_CHECK(
        gdt_test_access_byte(kernel_data) == 0x93,
        "kernel data access attributes"
    );

    GDT_TEST_CHECK(
        gdt_test_flags(kernel_data) == 0x0C,
        "kernel data flags"
    );

    /*
    * TSS:
    * P=1, DPL=0, system descriptor, busy 64-bit TSS.
    */
    GDT_TEST_CHECK(
        gdt_test_access_byte(tss_low) == 0x8B,
        "TSS descriptor access attributes"
    );

    /*
     * The high TSS descriptor contains the upper
     * 32 bits of the TSS base address.
     */
    GDT_TEST_CHECK(
        tss_high == (tss_high & 0xFFFFFFFFULL),
        "TSS upper descriptor reserved bits"
    );

    /*
     * User data:
     * P=1, DPL=3, S=1, writable data.
     */
    GDT_TEST_CHECK(
        gdt_test_access_byte(user_data) == 0xF3,
        "user data privilege and access attributes"
    );

    GDT_TEST_CHECK(
        gdt_test_flags(user_data) == 0x0C,
        "user data descriptor flags"
    );

    /*
     * User code:
     * P=1, DPL=3, S=1, executable and readable.
     */
    GDT_TEST_CHECK(
        gdt_test_access_byte(user_code) == 0xFB,
        "user code privilege and access attributes"
    );

    GDT_TEST_CHECK(
        gdt_test_flags(user_code) == 0x0A,
        "user code long-mode flags"
    );

    /*
     * Verify the selector values.
     */
    GDT_TEST_CHECK(
        GDT_KERNEL_CODE_SELECTOR == 0x08 &&
        GDT_KERNEL_DATA_SELECTOR == 0x10 &&
        GDT_TSS_SELECTOR == 0x18,
        "kernel and TSS selectors"
    );

    GDT_TEST_CHECK(
        GDT_USER_DATA_SELECTOR == 0x2B &&
        GDT_USER_CODE_SELECTOR == 0x33,
        "user selectors and RPL"
    );

    return true;
}


/*
 * Verify invalid GDT descriptor requests.
 */
static bool gdt_test_descriptor_bounds(void) {

    uint64_t value = 0x123456789ABCDEF0ULL;

    GDT_TEST_CHECK(
        !gdt_test_get_descriptor(
            GDT_ENTRY_COUNT,
            &value
        ),
        "descriptor index upper boundary rejection"
    );

    GDT_TEST_CHECK(
        !gdt_test_get_descriptor(
            UINT32_MAX,
            &value
        ),
        "invalid descriptor index rejection"
    );

    GDT_TEST_CHECK(
        value == 0x123456789ABCDEF0ULL,
        "invalid descriptor request preserves output"
    );

    GDT_TEST_CHECK(
        !gdt_test_get_descriptor(0, NULL),
        "null descriptor output rejection"
    );

    return true;
}


/*
 * Verify initial TSS.RSP0 configuration.
 */
static bool gdt_test_initial_kernel_stack(
    uintptr_t *initial_stack_out
) {

    if (initial_stack_out == NULL) {
        return false;
    }

    uintptr_t stack_top = 0;

    GDT_TEST_CHECK(
        gdt_get_kernel_stack(&stack_top),
        "initial TSS RSP0 readable"
    );

    GDT_TEST_CHECK(
        stack_top != 0,
        "initial TSS RSP0 nonzero"
    );

    GDT_TEST_CHECK(
        (stack_top & 0xFULL) == 0,
        "initial TSS RSP0 alignment"
    );

    GDT_TEST_CHECK(
        gdt_test_is_upper_canonical(stack_top),
        "initial TSS RSP0 canonical address"
    );

    *initial_stack_out = stack_top;

    GDT_TEST_CHECK(
        !gdt_get_kernel_stack(NULL),
        "null TSS stack output rejected"
    );

    return true;
}


/*
 * Verify invalid stack-top updates are rejected
 * without corrupting the currently installed RSP0.
 */
static bool gdt_test_invalid_stack_updates(
    uintptr_t original_stack
) {

    uintptr_t observed = 0;

    GDT_TEST_CHECK(
        !gdt_set_kernel_stack(0),
        "zero kernel stack rejected"
    );

    GDT_TEST_CHECK(
        !gdt_set_kernel_stack(original_stack + 1),
        "misaligned kernel stack rejected"
    );

    GDT_TEST_CHECK(
        !gdt_set_kernel_stack(0x0000000000100000ULL),
        "lower-half stack rejected"
    );

    GDT_TEST_CHECK(
        !gdt_set_kernel_stack(0x0000800000000000ULL),
        "noncanonical stack rejected"
    );

    GDT_TEST_CHECK(
        gdt_get_kernel_stack(&observed) &&
        observed == original_stack,
        "invalid updates preserve TSS RSP0"
    );

    return true;
}


/*
 * Verify that a valid alternate kernel stack can be
 * installed and that the dedicated bootstrap transition
 * stack can be restored afterward.
 *
 * The alternate stack is an actual mapped local stack
 * region; it is not an arbitrary fabricated address.
 */
static bool gdt_test_stack_switch_and_restore(
    uintptr_t original_stack
) {

    uint8_t alternate_stack[256]
        __attribute__((aligned(16)));

    uintptr_t alternate_top =
        (uintptr_t)(alternate_stack + sizeof(alternate_stack));

    GDT_TEST_CHECK(
        gdt_test_is_upper_canonical(alternate_top) &&
        (alternate_top & 0xFULL) == 0,
        "alternate test stack validity"
    );

    if (!gdt_set_kernel_stack(alternate_top)) {
        console_write(
            "[ FAILED ] GDT: alternate TSS RSP0 installation\n"
        );

        (void)gdt_restore_bootstrap_stack();

        return false;
    }

    uintptr_t observed = 0;

    bool alternate_installed =
        gdt_get_kernel_stack(&observed) &&
        observed == alternate_top;

    /*
     * Restore even if the subsequent readback fails.
     */
    bool restored =
        gdt_restore_bootstrap_stack();

    bool restore_verified =
        gdt_get_kernel_stack(&observed) &&
        observed == original_stack;

    if (!alternate_installed) {
        console_write(
            "[ FAILED ] GDT: alternate TSS RSP0 readback\n"
        );

        return false;
    }

    if (!restored || !restore_verified) {
        console_write(
            "[ FAILED ] GDT: bootstrap TSS RSP0 restoration\n"
        );

        return false;
    }

    return true;
}


/*
 * Run all GDT and TSS self-tests.
 */
bool gdt_run_tests(void) {

    uint16_t current_tr = 0;

    asm volatile (
        "str %0"
        : "=r"(current_tr)
    );

    GDT_TEST_CHECK(
        current_tr == GDT_TSS_SELECTOR,
        "hardware task register selector"
    );

    GDT_TEST_CHECK(
        gdt_test_descriptors(),
        "descriptor validation"
    );

    GDT_TEST_CHECK(
        gdt_test_descriptor_bounds(),
        "descriptor bounds"
    );

    uintptr_t original_stack = 0;

    GDT_TEST_CHECK(
        gdt_test_initial_kernel_stack(&original_stack),
        "initial kernel stack"
    );

    GDT_TEST_CHECK(
        gdt_test_invalid_stack_updates(original_stack),
        "invalid kernel stack updates"
    );

    GDT_TEST_CHECK(
        gdt_test_stack_switch_and_restore(original_stack),
        "dynamic kernel stack and restoration"
    );

    return true;
}

#undef GDT_TEST_CHECK
