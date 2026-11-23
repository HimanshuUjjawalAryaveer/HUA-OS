#include <stdbool.h>
#include <stdint.h>
#include "arch/x86_64/syscall.h"
#include "syscall/syscall_abi.h"
#include "arch/x86_64/gdt.h"
#include <stddef.h>
#include "arch/x86_64/syscall_frame.h"
#include "drivers/console/console.h"
#include "mm/pmm.h"

#include "tests/syscall_tests.h"


/*
 * Verify x86-64 syscall capability and HUA ABI definitions.
 *
 * No syscall MSRs are written here.
 * No privilege transition is performed here.
 */
bool syscall_run_tests(void) {

    /*
     * Test 1: CPU capability.
     */
    if (!arch_syscall_cpu_supported()) {
        return false;
    }

    /*
     * Test 2: ABI version.
     */
    if (HUA_SYSCALL_ABI_VERSION != 1U) {
        return false;
    }

    /*
     * Test 3: First syscall number.
     */
    if (HUA_SYSCALL_GET_ABI_VERSION != 0) {
        return false;
    }

    /*
     * Test 4: Verify every assigned syscall number is
     * consecutive and has not accidentally been reused.
     */
    if (HUA_SYSCALL_GET_PROCESS_ID !=
            HUA_SYSCALL_GET_ABI_VERSION + 1 ||

        HUA_SYSCALL_THREAD_YIELD !=
            HUA_SYSCALL_GET_PROCESS_ID + 1 ||

        HUA_SYSCALL_GET_PROCESS_INFO !=
            HUA_SYSCALL_THREAD_YIELD + 1 ||

        HUA_SYSCALL_THREAD_EXIT !=
            HUA_SYSCALL_GET_PROCESS_INFO + 1 ||

        HUA_SYSCALL_PROCESS_EXIT !=
            HUA_SYSCALL_THREAD_EXIT + 1) {

        return false;
    }

    /*
     * Test 5: COUNT must be one greater than the
     * highest assigned syscall number.
     */
    if (HUA_SYSCALL_COUNT !=
        HUA_SYSCALL_PROCESS_EXIT + 1) {

        return false;
    }

    return true;
}

/*
 * Verify the trusted syscall-entry stack state.
 *
 * This test requires:
 * - GDT/TSS initialization.
 * - Syscall state initialization.
 *
 * It does not execute SYSCALL.
 */
bool syscall_state_run_tests(void) {

    /*
     * The state must already be initialized.
     */
    if (!arch_syscall_state_is_initialized()) {
        return false;
    }

    /*
     * Retrieve the hardware TSS.RSP0.
     */
    uintptr_t tss_stack_top = 0;

    if (!gdt_get_kernel_stack(&tss_stack_top)) {
        return false;
    }

    /*
     * Retrieve HUA's trusted syscall stack.
     */
    uintptr_t syscall_stack_top = 0;

    if (!arch_syscall_get_kernel_stack(
            &syscall_stack_top
        )) {

        return false;
    }

    /*
     * Both stack pointers must match.
     */
    if (syscall_stack_top != tss_stack_top) {
        return false;
    }

    /*
     * Reject a NULL kernel stack.
     */
    if (arch_syscall_set_kernel_stack(0)) {
        return false;
    }

    /*
     * Reject an improperly aligned stack.
     */
    if (arch_syscall_set_kernel_stack(
            syscall_stack_top - sizeof(uintptr_t)
        )) {

        return false;
    }

    /*
     * Reject a valid-looking but incorrect stack address.
     *
     * This must not match the currently configured TSS.RSP0.
     */
    if (syscall_stack_top <= UINTPTR_MAX - 16) {

        if (arch_syscall_set_kernel_stack(
                syscall_stack_top + 16
            )) {

            return false;
        }
    }

    /*
     * Invalid update attempts must not corrupt the
     * previously published trusted stack.
     */
    uintptr_t stack_after_invalid_attempts = 0;

    if (!arch_syscall_get_kernel_stack(
            &stack_after_invalid_attempts
        )) {

        return false;
    }

    if (stack_after_invalid_attempts != syscall_stack_top) {
        return false;
    }

    /*
     * A valid update with the current TSS.RSP0 must succeed.
     */
    if (!arch_syscall_set_kernel_stack(syscall_stack_top)) {
        return false;
    }

    return true;
}


/*
 * Directly exercise the production SYSRETQ return-state
 * validator using the currently running user process.
 *
 * These are validation-only tests. They do not execute
 * invalid SYSRETQ instructions or deliberately fault.
 */
bool syscall_user_return_validation_run_tests(
    struct thread *thread,
    uintptr_t user_code,
    uintptr_t user_stack_top
) {

    console_write(
        "\n========== SYSCALL RETURN VALIDATION TESTS ==========\n"
    );

    if (thread == NULL ||
        user_code == 0 ||
        user_stack_top < (2ULL * PMM_PAGE_SIZE)) {

        console_write(
            "[ FAILED ] Validation test prerequisites\n"
        );

        return false;
    }

    struct arch_syscall_frame frame = {0};

    /*
     * Construct a valid baseline.
     *
     * user_code:
     *     User-readable, executable, read-only code mapping.
     *
     * user_stack_top:
     *     Top of the user-writable, NX stack mapping.
     *
     * RFLAGS:
     *     Reserved bit 1 set; interrupts disabled.
     */
    frame.rcx = (uint64_t)user_code;
    frame.r11 = 0x02ULL;
    frame.user_rsp = (uint64_t)user_stack_top;

    uint32_t passed = 0;

    if (!arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] Valid user return state rejected\n"
        );

        return false;
    }

    passed++;

    /*
     * IF is permitted in user return state.
     */
    frame.r11 = 0x202ULL;

    if (!arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] Valid user IF flag rejected\n"
        );

        return false;
    }

    passed++;

    /*
     * Arithmetic flags are also allowed.
     */
    frame.r11 = 0x246ULL;

    if (!arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] Valid arithmetic RFLAGS rejected\n"
        );

        return false;
    }

    passed++;

    /*
     * NULL thread and NULL frame must be rejected.
     */
    frame.r11 = 0x02ULL;

    if (arch_syscall_validate_user_return_state(
            NULL,
            &frame
        )) {

        console_write(
            "[ FAILED ] NULL thread accepted\n"
        );

        return false;
    }

    passed++;

    if (arch_syscall_validate_user_return_state(
            thread,
            NULL
        )) {

        console_write(
            "[ FAILED ] NULL syscall frame accepted\n"
        );

        return false;
    }

    passed++;

    /*
     * ----------------------------------------------------------------------
     * Invalid user RIP tests
     * ----------------------------------------------------------------------
     *
     * The RIP must be in a mapped, user-accessible,
     * executable, non-writable mapping.
     */
    const uint64_t invalid_rips[] = {

        0ULL,

        0x0000800000000000ULL,

        0x00007FFFFFFFFFFFULL,

        0xFFFF800000000000ULL,

        (uint64_t)user_code + PMM_PAGE_SIZE,

        (uint64_t)user_stack_top,

        (uint64_t)user_stack_top - PMM_PAGE_SIZE,

        (uint64_t)user_stack_top - (2ULL * PMM_PAGE_SIZE)
    };

    for (size_t i = 0;
         i < sizeof(invalid_rips) / sizeof(invalid_rips[0]);
         i++) {

        frame.rcx = invalid_rips[i];

        frame.r11 = 0x02ULL;

        frame.user_rsp = (uint64_t)user_stack_top;

        if (arch_syscall_validate_user_return_state(
                thread,
                &frame
            )) {

            console_write(
                "[ FAILED ] Invalid user RIP accepted\n"
            );

            return false;
        }

        passed++;
    }

    /*
     * A valid executable address at the end of the mapped
     * code page must remain accepted.
     */
    frame.rcx =
        (uint64_t)user_code + PMM_PAGE_SIZE - 1ULL;

    frame.r11 = 0x02ULL;

    frame.user_rsp = (uint64_t)user_stack_top;

    if (!arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] Last byte of valid code page rejected\n"
        );

        return false;
    }

    passed++;

    /*
     * ----------------------------------------------------------------------
     * Invalid user RSP tests
     * ----------------------------------------------------------------------
     *
     * The byte immediately below RSP must be mapped
     * user-writable and non-executable.
     */
    const uint64_t invalid_rsps[] = {

        0ULL,

        1ULL,

        0x0000800000000000ULL,

        0x00007FFFFFFFFFFFULL,

        0xFFFF800000000000ULL,

        (uint64_t)user_stack_top - PMM_PAGE_SIZE,

        (uint64_t)user_stack_top - (2ULL * PMM_PAGE_SIZE),

        (uint64_t)user_code,

        (uint64_t)user_stack_top + 1ULL,

        (uint64_t)user_stack_top + PMM_PAGE_SIZE
    };

    for (size_t i = 0;
         i < sizeof(invalid_rsps) / sizeof(invalid_rsps[0]);
         i++) {

        frame.rcx = (uint64_t)user_code;

        frame.r11 = 0x02ULL;

        frame.user_rsp = invalid_rsps[i];

        if (arch_syscall_validate_user_return_state(
                thread,
                &frame
            )) {

            console_write(
                "[ FAILED ] Invalid user RSP accepted\n"
            );

            return false;
        }

        passed++;
    }

    /*
     * RSP-1 must not refer to executable, read-only code.
     */
    frame.rcx = (uint64_t)user_code;

    frame.r11 = 0x02ULL;

    frame.user_rsp = (uint64_t)user_code + 1ULL;

    if (arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] Read-only code accepted as user stack\n"
        );

        return false;
    }

    passed++;

    /*
     * The top of the valid user stack is accepted.
     */
    frame.user_rsp = (uint64_t)user_stack_top;

    if (!arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] Valid user stack top rejected\n"
        );

        return false;
    }

    passed++;

    /*
     * ----------------------------------------------------------------------
     * Forbidden RFLAGS tests
     * ----------------------------------------------------------------------
     *
     * Each flag is tested independently so that one
     * forbidden flag cannot hide another missing check.
     */
    const uint64_t forbidden_flags[] = {

        (1ULL << 8),   /* TF   */

        (1ULL << 10),  /* DF   */

        (1ULL << 12),  /* IOPL bit 0 */

        (1ULL << 13),  /* IOPL bit 1 */

        (1ULL << 14),  /* NT   */

        (1ULL << 16),  /* RF   */

        (1ULL << 17),  /* VM   */

        (1ULL << 18),  /* AC   */

        (1ULL << 19),  /* VIF  */

        (1ULL << 20)   /* VIP  */
    };

    for (size_t i = 0;
         i < sizeof(forbidden_flags) / sizeof(forbidden_flags[0]);
         i++) {

        frame.rcx = (uint64_t)user_code;

        frame.user_rsp = (uint64_t)user_stack_top;

        /*
         * Keep reserved bit 1 set while injecting
         * exactly one forbidden flag.
         */
        frame.r11 = 0x02ULL | forbidden_flags[i];

        if (arch_syscall_validate_user_return_state(
                thread,
                &frame
            )) {

            console_write(
                "[ FAILED ] Forbidden user RFLAGS accepted\n"
            );

            return false;
        }

        passed++;
    }

    /*
     * Bit 1 is mandatory.
     */
    frame.r11 = 0;

    if (arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] RFLAGS without reserved bit 1 accepted\n"
        );

        return false;
    }

    passed++;

    /*
     * Restore a valid baseline before returning.
     */
    frame.rcx = (uint64_t)user_code;

    frame.r11 = 0x02ULL;

    frame.user_rsp = (uint64_t)user_stack_top;

    if (!arch_syscall_validate_user_return_state(
            thread,
            &frame
        )) {

        console_write(
            "[ FAILED ] Validator did not recover after negative tests\n"
        );

        return false;
    }

    /* Count the successful baseline-recovery test. */
    passed++;

    /* Verify that all 38 cases ran successfully. */
    if (passed != 38U) {

        console_write(
            "[ FAILED ] Unexpected validation test count\n"
        );

        return false;
    }

    console_write(
        "[ OK ] Invalid RIP cases rejected\n"
    );

    console_write(
        "[ OK ] Invalid RSP cases rejected\n"
    );

    console_write(
        "[ OK ] Forbidden RFLAGS cases rejected\n"
    );

    console_write(
        "[ OK ] Valid user return states accepted\n"
    );

    console_write(
        "[ OK ] All 38 return-state validation cases passed\n"
    );

    console_write(
        "========== SYSCALL RETURN VALIDATION TESTS PASSED ==========\n"
    );

    return true;
}
