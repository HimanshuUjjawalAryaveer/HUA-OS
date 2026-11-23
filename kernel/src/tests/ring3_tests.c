#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/ring3.h"
#include "drivers/console/console.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "process/process.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "arch/x86_64/syscall.h"
#include "syscall/syscall_abi.h"

#include "tests/ring3_tests.h"
#include "tests/syscall_tests.h"
#include "tests/user_memory_tests.h"


/*
 * Assembly/C handoff slot used by syscall_entry.asm.
 *
 * The test verifies that it is cleared after the
 * entire Ring 3 syscall stress program has returned.
 */
extern volatile uintptr_t arch_syscall_user_rsp_scratch;

/* -------------------------------------------------------------------------- */
/* Test configuration                                                         */
/* -------------------------------------------------------------------------- */

/*
 * One user RX code page.
 */
#define RING3_TEST_CODE_VA \
    0x0000000040000000ULL

/*
 * One user RW/NX stack page.
 */
#define RING3_TEST_STACK_VA \
    0x0000000070000000ULL

#define RING3_TEST_STACK_TOP \
    (RING3_TEST_STACK_VA + PMM_PAGE_SIZE)

/*
 * Writable destination for hua_process_info_t.
 *
 * The structure occupies:
 *
 *     [RSP - 112] through [RSP - 97]
 *
 * It does not overlap the existing syscall-result slots.
 */
#define RING3_TEST_PROCESS_INFO_USER_VA \
    (RING3_TEST_STACK_TOP - 112ULL)

/*
 * Read-only destination at the end of the RX code page.
 *
 * The generated program must not overlap this region.
 */
#define RING3_TEST_PROCESS_INFO_READONLY_VA \
    (RING3_TEST_CODE_VA + PMM_PAGE_SIZE - \
     sizeof(hua_process_info_t))

/*
 * An intentionally unmapped lower guard-page address.
 */
#define RING3_TEST_UNMAPPED_USER_VA \
    (RING3_TEST_STACK_VA - PMM_PAGE_SIZE)

/*
 * Address used for the supervisor-only mapping test.
 *
 * The page is present, read-only, NX, and supervisor-only.
 */
#define RING3_TEST_SUPERVISOR_VA \
    (RING3_TEST_CODE_VA + PMM_PAGE_SIZE)

/*
 * Target of the attempted write to the read-only code page.
 * Offset 0x100 keeps this separate from generated instructions.
 */
#define RING3_TEST_READONLY_WRITE_VA \
    (RING3_TEST_CODE_VA + 0x100ULL)

/*
 * Starts eight bytes before the end of the writable stack.
 *
 * A 16-byte write therefore crosses into the unmapped
 * page following the stack.
 */
#define RING3_TEST_CROSS_PAGE_USER_VA \
    (RING3_TEST_STACK_TOP - 8ULL)

/*
 * Maximum time to wait for the worker and its deferred
 * reaping operation.
 *
 * Measured in LAPIC timer ticks.
 */
#define RING3_TEST_TIMEOUT_TICKS 2000ULL

#define RING3_TEST_ABI_STRESS_COUNT   128U
#define RING3_TEST_YIELD_STRESS_COUNT 32U

#define RING3_TEST_MIN_BOOTSTRAP_RESUMPTIONS \
    (RING3_TEST_YIELD_STRESS_COUNT - 1U)

#define RING3_TEST_CODE_CAPACITY PMM_PAGE_SIZE

#define RING3_TEST_FAIL_JUMP_CAPACITY 256U

/*
 * CR3 physical root mask.
 */
#define RING3_TEST_CR3_MASK \
    0x000FFFFFFFFFF000ULL

#define RING3_TEST_CROSS_PAGE_SENTINEL 0x13579BDFULL

/*
 * Deliberately use a negative 64-bit value whose magnitude
 * does not fit in 32 bits.
 *
 * This verifies that RDI is preserved as a complete signed
 * 64-bit syscall argument.
 */
#define RING3_TEST_THREAD_EXIT_EXPECTED_STATUS \
    (-INT64_C(0x123456789))

#define RING3_TEST_PROCESS_EXIT_EXPECTED_STATUS \
    (-INT64_C(0x23456789A))

/*
 * Distinct failure status for exception-test setup.
 *
 * The real exception termination path must use -1.
 * A different value makes setup failures detectable.
 */
#define RING3_TEST_EXCEPTION_FAILURE_STATUS \
    (-INT64_C(0x3456789AB))

#define RING3_TEST_EXCEPTION_EXIT_STATUS (-1LL)

/*
 * Page-fault test accesses the lower unmapped guard page.
 */
#define RING3_TEST_EXCEPTION_FAULT_VA \
    RING3_TEST_UNMAPPED_USER_VA

/*
 * Distinct failure result used when the kernel-side test setup
 * fails before the user-mode exit syscall is executed.
 */
#define RING3_TEST_THREAD_EXIT_FAILURE_STATUS \
    (-INT64_C(0x7654321))

/*
 * Dedicated kernel entry stack for the controlled Ring 3 test.
 *
 * The test worker has suspended C frames on its ordinary
 * thread stack while executing user code.
 *
 * Syscalls and Ring 3 privilege transitions must therefore
 * use this separate stack during the test.
 *
 * CPU0 only; one active Ring 3 test at a time.
 */
#define RING3_TEST_SYSCALL_STACK_SIZE (32U * 1024U)

/*
 * Kernel-to-survivor control page.
 *
 * Mapped into the survivor as user-readable, read-only, NX.
 * Only the kernel changes its contents through the HHDM.
 */
#define RING3_TEST_ISOLATION_CONTROL_VA \
    0x0000000060000000ULL

/*
 * Survivor-to-kernel status page.
 *
 * Mapped into the survivor as user-readable, writable, NX.
 */
#define RING3_TEST_ISOLATION_STATUS_VA \
    0x0000000061000000ULL

#define RING3_TEST_ISOLATION_B_STARTED \
    0xB57A0001U

#define RING3_TEST_ISOLATION_A_REAPED \
    0xA11CE001U

#define RING3_TEST_ISOLATION_ABORTED \
    0xDEAD0001U

#define RING3_TEST_ISOLATION_B_ACKNOWLEDGED \
    0xBACC0001U

#define RING3_TEST_ISOLATION_SURVIVOR_FAILURE_STATUS \
    (-INT64_C(0x456789ABC))

struct ring3_test_isolation_control {

    uint32_t faulting_reaped;

    uint32_t reserved[3];
};

struct ring3_test_isolation_status {

    uint32_t survivor_started;

    uint32_t survivor_acknowledged;

    uint32_t reserved[2];
};

_Static_assert(
    sizeof(struct ring3_test_isolation_control) == 16,
    "Isolation control structure must be 16 bytes"
);

_Static_assert(
    sizeof(struct ring3_test_isolation_status) == 16,
    "Isolation status structure must be 16 bytes"
);

_Static_assert(
    offsetof(
        struct ring3_test_isolation_control,
        faulting_reaped
    ) == 0,
    "Isolation faulting-reaped offset mismatch"
);

_Static_assert(
    offsetof(
        struct ring3_test_isolation_status,
        survivor_acknowledged
    ) == 4,
    "Isolation acknowledgement offset mismatch"
);

typedef enum {

    RING3_EXCEPTION_TEST_DIVIDE_ERROR = 0,
    RING3_EXCEPTION_TEST_INVALID_OPCODE,
    RING3_EXCEPTION_TEST_GENERAL_PROTECTION,
    RING3_EXCEPTION_TEST_UNMAPPED_READ,
    RING3_EXCEPTION_TEST_READONLY_WRITE,
    RING3_EXCEPTION_TEST_NX_EXECUTE,
    RING3_EXCEPTION_TEST_SUPERVISOR_READ

} ring3_exception_test_case_t;

static uint8_t ring3_test_syscall_stack[
    RING3_TEST_SYSCALL_STACK_SIZE
] __attribute__((aligned(4096)));


/* -------------------------------------------------------------------------- */
/* Ring 3 syscall torture-test machine-code builder                           */
/* -------------------------------------------------------------------------- */

struct ring3_test_code_builder {

    uint8_t *bytes;

    size_t capacity;

    size_t length;

    size_t fail_jumps[RING3_TEST_FAIL_JUMP_CAPACITY];

    size_t fail_jump_count;

    bool failed;
};


/*
 * Emit one byte.
 */
static bool ring3_code_emit8(
    struct ring3_test_code_builder *builder,
    uint8_t value
) {

    if (builder == NULL ||
        builder->failed ||
        builder->length >= builder->capacity) {

        if (builder != NULL) {
            builder->failed = true;
        }

        return false;
    }

    builder->bytes[builder->length++] = value;

    return true;
}


/*
 * Emit a little-endian 32-bit immediate.
 */
static bool ring3_code_emit32(
    struct ring3_test_code_builder *builder,
    uint32_t value
) {

    for (unsigned int i = 0; i < 4; i++) {

        if (!ring3_code_emit8(
                builder,
                (uint8_t)(value >> (i * 8U))
            )) {

            return false;
        }
    }

    return true;
}


/*
 * Emit a little-endian 64-bit immediate.
 */
static bool ring3_code_emit64(
    struct ring3_test_code_builder *builder,
    uint64_t value
) {

    for (unsigned int i = 0; i < 8; i++) {

        if (!ring3_code_emit8(
                builder,
                (uint8_t)(value >> (i * 8U))
            )) {

            return false;
        }
    }

    return true;
}


/*
 * Emit a byte sequence.
 */
static bool ring3_code_emit_bytes(
    struct ring3_test_code_builder *builder,
    const uint8_t *bytes,
    size_t count
) {

    if (builder == NULL ||
        bytes == NULL ||
        count > builder->capacity - builder->length) {

        if (builder != NULL) {
            builder->failed = true;
        }

        return false;
    }

    for (size_t i = 0; i < count; i++) {
        builder->bytes[builder->length++] = bytes[i];
    }

    return true;
}


/*
 * Patch a signed 32-bit relative branch displacement.
 */
static bool ring3_code_patch_rel32(
    struct ring3_test_code_builder *builder,
    size_t displacement_offset,
    size_t target
) {

    if (builder == NULL ||
        displacement_offset > builder->length ||
        builder->length - displacement_offset < 4U) {

        return false;
    }

    int64_t relative =
        (int64_t)target -
        (int64_t)(displacement_offset + 4U);

    if (relative < INT32_MIN ||
        relative > INT32_MAX) {

        return false;
    }

    uint32_t encoded = (uint32_t)(int32_t)relative;

    for (unsigned int i = 0; i < 4; i++) {

        builder->bytes[displacement_offset + i] =
            (uint8_t)(encoded >> (i * 8U));
    }

    return true;
}


/*
 * Emit JNE with a placeholder displacement.
 *
 * The destination is patched after the failure block
 * has been emitted.
 */
static size_t ring3_code_emit_jne_placeholder(
    struct ring3_test_code_builder *builder
) {

    if (!ring3_code_emit8(builder, 0x0F) ||
        !ring3_code_emit8(builder, 0x85)) {

        return SIZE_MAX;
    }

    size_t displacement_offset = builder->length;

    if (!ring3_code_emit32(builder, 0)) {
        return SIZE_MAX;
    }

    return displacement_offset;
}


/*
 * Emit JNE to the shared failure block.
 */
static bool ring3_code_emit_jne_fail(
    struct ring3_test_code_builder *builder
) {

    if (builder == NULL ||
        builder->fail_jump_count >=
            RING3_TEST_FAIL_JUMP_CAPACITY) {

        return false;
    }

    size_t displacement =
        ring3_code_emit_jne_placeholder(builder);

    if (displacement == SIZE_MAX) {
        return false;
    }

    builder->fail_jumps[
        builder->fail_jump_count++
    ] = displacement;

    return true;
}


/*
 * Emit JNE to a known target.
 */
static bool ring3_code_emit_jne_target(
    struct ring3_test_code_builder *builder,
    size_t target
) {

    size_t displacement =
        ring3_code_emit_jne_placeholder(builder);

    if (displacement == SIZE_MAX) {
        return false;
    }

    return ring3_code_patch_rel32(
        builder,
        displacement,
        target
    );
}


/*
 * Emit an unconditional JMP to a known target.
 *
 * The target is patched later.
 */
static size_t ring3_code_emit_jmp_placeholder(
    struct ring3_test_code_builder *builder
) {

    if (!ring3_code_emit8(builder, 0xE9)) {
        return SIZE_MAX;
    }

    size_t displacement_offset = builder->length;

    if (!ring3_code_emit32(builder, 0)) {
        return SIZE_MAX;
    }

    return displacement_offset;
}


/*
 * Emit:
 *
 *     mov eax, immediate
 */
static bool ring3_code_emit_mov_eax(
    struct ring3_test_code_builder *builder,
    uint32_t immediate
) {

    return
        ring3_code_emit8(builder, 0xB8) &&
        ring3_code_emit32(builder, immediate);
}


/*
 * Emit:
 *
 *     syscall
 */
static bool ring3_code_emit_syscall(
    struct ring3_test_code_builder *builder
) {

    return
        ring3_code_emit8(builder, 0x0F) &&
        ring3_code_emit8(builder, 0x05);
}


/*
 * Emit:
 *
 *     mov dword [rsp + displacement], immediate
 *
 * The displacement is an 8-bit signed displacement.
 */
static bool ring3_code_emit_mov_dword_rsp(
    struct ring3_test_code_builder *builder,
    uint8_t displacement,
    uint32_t immediate
) {

    return
        ring3_code_emit8(builder, 0xC7) &&
        ring3_code_emit8(builder, 0x44) &&
        ring3_code_emit8(builder, 0x24) &&
        ring3_code_emit8(builder, displacement) &&
        ring3_code_emit32(builder, immediate);
}


/*
 * Emit:
 *
 *     mov [rsp + displacement], rax
 */
static bool ring3_code_emit_store_rax_rsp(
    struct ring3_test_code_builder *builder,
    uint8_t displacement
) {

    return
        ring3_code_emit8(builder, 0x48) &&
        ring3_code_emit8(builder, 0x89) &&
        ring3_code_emit8(builder, 0x44) &&
        ring3_code_emit8(builder, 0x24) &&
        ring3_code_emit8(builder, displacement);
}


/*
 * Emit a register canary initialization.
 *
 * These values are deliberately different so that
 * accidental register swaps are detected too.
 */
static bool ring3_code_emit_register_canaries(
    struct ring3_test_code_builder *builder
) {

    struct register_initializer {

        uint8_t instruction[2];

        uint8_t instruction_size;

        uint32_t value;

    };

    static const struct register_initializer initializers[] = {

        { { 0xBB, 0x00 }, 1, 0x13579BDFU }, /* EBX */

        { { 0xBD, 0x00 }, 1, 0x2468ACE0U }, /* EBP */

        { { 0x41, 0xBC }, 2, 0xA12B3C4DU }, /* R12D */

        { { 0x41, 0xBD }, 2, 0xB23C4D5EU }, /* R13D */

        { { 0x41, 0xBE }, 2, 0xC34D5E6FU }, /* R14D */

        { { 0x41, 0xBF }, 2, 0xD45E6F70U }, /* R15D */

        { { 0xBF, 0x00 }, 1, 0x11112222U }, /* EDI */

        { { 0xBE, 0x00 }, 1, 0x22223333U }, /* ESI */

        { { 0xBA, 0x00 }, 1, 0x33334444U }, /* EDX */

        { { 0x41, 0xB8 }, 2, 0x44445555U }, /* R8D */

        { { 0x41, 0xB9 }, 2, 0x55556666U }, /* R9D */

        { { 0x41, 0xBA }, 2, 0x66667777U }  /* R10D */
    };

    for (size_t i = 0;
         i < sizeof(initializers) / sizeof(initializers[0]);
         i++) {

        if (!ring3_code_emit_bytes(
                builder,
                initializers[i].instruction,
                initializers[i].instruction_size
            ) ||
            !ring3_code_emit32(
                builder,
                initializers[i].value
            )) {

            return false;
        }
    }

    return true;
}


/*
 * Emit comparisons for every register canary.
 */
static bool ring3_code_emit_register_checks(
    struct ring3_test_code_builder *builder
) {

    struct register_check {

        uint8_t instruction[3];

        uint8_t instruction_size;

        uint32_t expected;

    };

    static const struct register_check checks[] = {

        { { 0x81, 0xFB, 0x00 }, 2, 0x13579BDFU }, /* EBX */

        { { 0x81, 0xFD, 0x00 }, 2, 0x2468ACE0U }, /* EBP */

        { { 0x41, 0x81, 0xFC }, 3, 0xA12B3C4DU }, /* R12D */

        { { 0x41, 0x81, 0xFD }, 3, 0xB23C4D5EU }, /* R13D */

        { { 0x41, 0x81, 0xFE }, 3, 0xC34D5E6FU }, /* R14D */

        { { 0x41, 0x81, 0xFF }, 3, 0xD45E6F70U }, /* R15D */

        { { 0x81, 0xFF, 0x00 }, 2, 0x11112222U }, /* EDI */

        { { 0x81, 0xFE, 0x00 }, 2, 0x22223333U }, /* ESI */

        { { 0x81, 0xFA, 0x00 }, 2, 0x33334444U }, /* EDX */

        { { 0x41, 0x81, 0xF8 }, 3, 0x44445555U }, /* R8D */

        { { 0x41, 0x81, 0xF9 }, 3, 0x55556666U }, /* R9D */

        { { 0x41, 0x81, 0xFA }, 3, 0x66667777U }  /* R10D */
    };

    for (size_t i = 0;
         i < sizeof(checks) / sizeof(checks[0]);
         i++) {

        if (!ring3_code_emit_bytes(
                builder,
                checks[i].instruction,
                checks[i].instruction_size
            ) ||
            !ring3_code_emit32(
                builder,
                checks[i].expected
            ) ||
            !ring3_code_emit_jne_fail(builder)) {

            return false;
        }
    }

    /*
     * Verify that SYSRETQ restored the original user RSP.
     *
     * At program start:
     *
     *     [rsp - 56] = original RSP
     *
     * After each syscall:
     *
     *     RAX = current RSP
     *     compare RAX with [rsp - 56]
     */
    static const uint8_t rsp_check[] = {

        0x48, 0x8D, 0x04, 0x24,

        0x48, 0x3B, 0x44, 0x24, 0xC8
    };

    return
        ring3_code_emit_bytes(
            builder,
            rsp_check,
            sizeof(rsp_check)
        ) &&
        ring3_code_emit_jne_fail(builder);
}

/*
 * Emit:
 *
 *     mov rdi, immediate_64
 *
 * RDI is syscall argument 1.
 */
static bool ring3_code_emit_mov_rdi_imm64(
    struct ring3_test_code_builder *builder,
    uint64_t value
) {

    return
        ring3_code_emit8(builder, 0x48) &&
        ring3_code_emit8(builder, 0xBF) &&
        ring3_code_emit64(builder, value);
}


/*
 * Restore the original EDI canary after a syscall test.
 *
 * The process-info syscall deliberately changes RDI to
 * supply its pointer argument. Restore the canary before
 * running the existing complete register checks.
 */
static bool ring3_code_emit_restore_edi_canary(
    struct ring3_test_code_builder *builder
) {

    return
        ring3_code_emit8(builder, 0xBF) &&
        ring3_code_emit32(builder, 0x11112222U);
}


/*
 * Emit one GET_PROCESS_INFO test using an invalid
 * user destination.
 *
 * Expected result:
 *
 *     RAX = -14
 *
 * The syscall must reject the pointer without crashing
 * the kernel or returning to an unsafe user state.
 *
 * result_displacement is the signed RSP-relative
 * byte displacement used to preserve the syscall result.
 */
static bool ring3_code_emit_process_info_bad_pointer_test(
    struct ring3_test_code_builder *builder,
    uint64_t user_pointer,
    uint8_t result_displacement
) {

    static const uint8_t compare_bad_address[] = {

        /*
         * cmp rax, -14
         *
         * 0xF2 is sign-extended to -14.
         */
        0x48, 0x83, 0xF8, 0xF2
    };

    return

        ring3_code_emit_mov_eax(
            builder,
            HUA_SYSCALL_GET_PROCESS_INFO
        ) &&

        ring3_code_emit_mov_rdi_imm64(
            builder,
            user_pointer
        ) &&

        ring3_code_emit_syscall(builder) &&

        ring3_code_emit_bytes(
            builder,
            compare_bad_address,
            sizeof(compare_bad_address)
        ) &&

        ring3_code_emit_jne_fail(builder) &&

        ring3_code_emit_store_rax_rsp(
            builder,
            result_displacement
        ) &&

        ring3_code_emit_restore_edi_canary(builder) &&

        ring3_code_emit_register_checks(builder);
}

/*
 * Prepare a deterministic sentinel for the cross-page
 * GET_PROCESS_INFO test.
 *
 * Preserve the previous ABI-version result at RSP - 128.
 * Then place a distinctive sentinel at RSP - 8, which is
 * the first eight bytes of the cross-page destination.
 *
 * If copy_to_user() partially writes the first page before
 * discovering that the second page is unmapped, the sentinel
 * will be overwritten.
 */
static bool ring3_code_emit_prepare_cross_page_sentinel(
    struct ring3_test_code_builder *builder
) {

    static const uint8_t instructions[] = {

        /*
         * mov rax, [rsp - 8]
         */
        0x48, 0x8B, 0x44, 0x24, 0xF8,

        /*
         * mov [rsp - 128], rax
         *
         * 0x80 is the signed 8-bit displacement -128.
         */
        0x48, 0x89, 0x44, 0x24, 0x80,

        /*
         * mov qword [rsp - 8], 0x13579BDF
         */
        0x48, 0xC7, 0x44, 0x24, 0xF8,
        0xDF, 0x9B, 0x57, 0x13
    };

    return ring3_code_emit_bytes(
        builder,
        instructions,
        sizeof(instructions)
    );
}

/*
 * Emit an invalid syscall test using a 32-bit syscall number.
 */
static bool ring3_code_emit_invalid_eax_syscall(
    struct ring3_test_code_builder *builder,
    uint32_t number
) {

    static const uint8_t compare_error[] = {
        0x48, 0x83, 0xF8, 0xDA
    };

    return
        ring3_code_emit_mov_eax(builder, number) &&
        ring3_code_emit_syscall(builder) &&

        /*
         * cmp rax, -38
         */
        ring3_code_emit_bytes(
            builder,
            compare_error,
            sizeof(compare_error)
        ) &&

        ring3_code_emit_jne_fail(builder) &&

        ring3_code_emit_store_rax_rsp(
            builder,
            0xE0
        ) &&

        ring3_code_emit_register_checks(builder);
}


/*
 * Emit an invalid syscall test using a full 64-bit number.
 */
static bool ring3_code_emit_invalid_rax_syscall(
    struct ring3_test_code_builder *builder,
    uint64_t number
) {

    static const uint8_t mov_rax_imm64[] = {
        0x48, 0xB8
    };

    static const uint8_t compare_error[] = {
        0x48, 0x83, 0xF8, 0xDA
    };

    return
        ring3_code_emit_bytes(
            builder,
            mov_rax_imm64,
            sizeof(mov_rax_imm64)
        ) &&

        ring3_code_emit64(builder, number) &&

        ring3_code_emit_syscall(builder) &&

        ring3_code_emit_bytes(
            builder,
            compare_error,
            sizeof(compare_error)
        ) &&

        ring3_code_emit_jne_fail(builder) &&

        ring3_code_emit_store_rax_rsp(
            builder,
            0xE0
        ) &&

        ring3_code_emit_register_checks(builder);
}


/*
 * Build the complete Ring 3 torture-test program.
 *
 * Results are stored below the initial user RSP:
 *
 *     RSP -  8: ABI version
 *     RSP - 16: process ID
 *     RSP - 24: last yield result
 *     RSP - 32: last invalid syscall result
 *     RSP - 40: ABI loop counter
 *     RSP - 48: yield loop counter
 *     RSP - 56: original RSP snapshot
 *     RSP - 64: overall test status (0 = pass)
 *
 * The final instruction is INT3, which returns through
 * HUA's existing controlled Ring 3 breakpoint handler.
 */
static bool ring3_test_build_user_program(
    uint8_t *output,
    size_t capacity,
    size_t *program_size_out,
    size_t *breakpoint_offset_out
) {

    if (output == NULL ||
        program_size_out == NULL ||
        breakpoint_offset_out == NULL ||
        capacity == 0) {

        return false;
    }

    struct ring3_test_code_builder builder = {
        .bytes = output,
        .capacity = capacity,
        .length = 0,
        .fail_jump_count = 0,
        .failed = false
    };

    /*
     * Default to failure.
     *
     * The user program overwrites this with zero only
     * after every test has succeeded.
     */
    if (!ring3_code_emit_mov_dword_rsp(
            &builder,
            0xC0,
            1
        )) {

        return false;
    }

    /*
     * Preserve the initial user RSP for integrity checks.
     */
    static const uint8_t save_rsp[] = {
        0x48, 0x8D, 0x04, 0x24,
        0x48, 0x89, 0x44, 0x24, 0xC8
    };

    if (!ring3_code_emit_bytes(
            &builder,
            save_rsp,
            sizeof(save_rsp)
        ) ||
        !ring3_code_emit_register_canaries(&builder)) {

        return false;
    }

    /*
     * Initialize the ABI stress counter:
     *
     *     [rsp - 40] = 128
     */
    if (!ring3_code_emit_mov_dword_rsp(
            &builder,
            0xD8,
            RING3_TEST_ABI_STRESS_COUNT
        )) {

        return false;
    }

    /*
     * Repeated ABI-version syscall.
     */
    size_t abi_loop = builder.length;

    if (!ring3_code_emit_mov_eax(
            &builder,
            HUA_SYSCALL_GET_ABI_VERSION
        ) ||
        !ring3_code_emit_syscall(&builder)) {

        return false;
    }

    /*
     * cmp eax, HUA_SYSCALL_ABI_VERSION
     */
    if (!ring3_code_emit8(&builder, 0x83) ||
        !ring3_code_emit8(&builder, 0xF8) ||
        !ring3_code_emit8(&builder, HUA_SYSCALL_ABI_VERSION) ||
        !ring3_code_emit_jne_fail(&builder) ||

        !ring3_code_emit_store_rax_rsp(
            &builder,
            0xF8
        ) ||

        !ring3_code_emit_register_checks(&builder) ||

        /*
         * dec dword [rsp - 40]
         */
        !ring3_code_emit8(&builder, 0xFF) ||
        !ring3_code_emit8(&builder, 0x4C) ||
        !ring3_code_emit8(&builder, 0x24) ||
        !ring3_code_emit8(&builder, 0xD8) ||

        !ring3_code_emit_jne_target(
            &builder,
            abi_loop
        )) {

        return false;
    }

    /*
     * Process-ID syscall.
     *
     * The argument registers deliberately contain
     * canary values. The kernel must obtain the PID
     * from the current thread's owning process.
     */
    if (!ring3_code_emit_mov_eax(
            &builder,
            HUA_SYSCALL_GET_PROCESS_ID
        ) ||
        !ring3_code_emit_syscall(&builder) ||

        !ring3_code_emit_store_rax_rsp(
            &builder,
            0xF0
        ) ||

        !ring3_code_emit_register_checks(&builder)) {

        return false;
    }

        /*
     * ----------------------------------------------------------------------
     * GET_PROCESS_INFO syscall
     * ----------------------------------------------------------------------
     *
     * User destination:
     *
     *     RSP - 112 through RSP - 97
     *
     * Result slots:
     *
     *     RSP - 72  : valid destination result
     *     RSP - 80  : NULL destination result
     *     RSP - 88  : unmapped destination result
     *     RSP - 96  : read-only destination result
     *     RSP - 120 : cross-page destination result
     *
     * The output structure is initialized with recognizable
     * values so that the verifier can check the returned
     * contents rather than relying on zero-initialized memory.
     */

    /*
     * Initialize all four 32-bit structure fields.
     *
     * The syscall must replace every sentinel.
     */
    if (!ring3_code_emit_mov_dword_rsp(
            &builder,
            0x90,       /* RSP - 112 */
            0xA1A1A1A1U
        ) ||
        !ring3_code_emit_mov_dword_rsp(
            &builder,
            0x94,       /* RSP - 108 */
            0xB2B2B2B2U
        ) ||
        !ring3_code_emit_mov_dword_rsp(
            &builder,
            0x98,       /* RSP - 104 */
            0xC3C3C3C3U
        ) ||
        !ring3_code_emit_mov_dword_rsp(
            &builder,
            0x9C,       /* RSP - 100 */
            0xD4D4D4D4U
        )) {

        return false;
    }

    /*
     * Valid destination test.
     *
     * RDI = RING3_TEST_PROCESS_INFO_USER_VA
     *
     * Expected:
     *
     *     RAX = 0
     */
    if (!ring3_code_emit_mov_eax(
            &builder,
            HUA_SYSCALL_GET_PROCESS_INFO
        ) ||
        !ring3_code_emit_mov_rdi_imm64(
            &builder,
            RING3_TEST_PROCESS_INFO_USER_VA
        ) ||
        !ring3_code_emit_syscall(&builder)) {

        return false;
    }

    /*
     * test rax, rax
     *
     * A successful process-info syscall must return zero.
     */
    if (!ring3_code_emit8(&builder, 0x48) ||
        !ring3_code_emit8(&builder, 0x85) ||
        !ring3_code_emit8(&builder, 0xC0) ||
        !ring3_code_emit_jne_fail(&builder) ||

        !ring3_code_emit_store_rax_rsp(
            &builder,
            0xB8        /* RSP - 72 */
        ) ||

        !ring3_code_emit_restore_edi_canary(&builder) ||

        !ring3_code_emit_register_checks(&builder)) {

        return false;
    }

    /*
     * NULL destination.
     *
     * Address zero must not be accepted as a writable
     * process-information destination.
     */
    if (!ring3_code_emit_process_info_bad_pointer_test(
            &builder,
            0,
            0xB0        /* RSP - 80 */
        )) {

        return false;
    }

    /*
     * Entirely unmapped destination.
     *
     * This uses the existing guard page below the stack.
     */
    if (!ring3_code_emit_process_info_bad_pointer_test(
            &builder,
            RING3_TEST_UNMAPPED_USER_VA,
            0xA8        /* RSP - 88 */
        )) {

        return false;
    }

    /*
     * Read-only destination.
     *
     * The destination is inside the RX user code page.
     */
    if (!ring3_code_emit_process_info_bad_pointer_test(
            &builder,
            RING3_TEST_PROCESS_INFO_READONLY_VA,
            0xA0        /* RSP - 96 */
        )) {

        return false;
    }

    /*
    * Install the cross-page sentinel immediately before
    * generating the cross-page syscall.
    *
    * Preserve the original ABI-version result separately.
    */
    if (!ring3_code_emit_prepare_cross_page_sentinel(
            &builder
        )) {

        return false;
    }

    /*
     * Cross-page destination.
     *
     * The first eight bytes are writable.
     * The following eight bytes belong to an unmapped page.
     *
     * copy_to_user() must preflight the COMPLETE range before
     * modifying the first page.
     *
     * The first eight bytes overlap the ABI-version result
     * at RSP - 8. The verifier checks that value afterward,
     * detecting an accidental partial write.
     */
    if (!ring3_code_emit_process_info_bad_pointer_test(
            &builder,
            RING3_TEST_CROSS_PAGE_USER_VA,
            0x88        /* RSP - 120 */
        )) {

        return false;
    }

    /*
     * Initialize the yield stress counter:
     *
     *     [rsp - 48] = 32
     */
    if (!ring3_code_emit_mov_dword_rsp(
            &builder,
            0xD0,
            RING3_TEST_YIELD_STRESS_COUNT
        )) {

        return false;
    }

    /*
     * Repeated yielding syscall.
     */
    size_t yield_loop = builder.length;

    if (!ring3_code_emit_mov_eax(
            &builder,
            HUA_SYSCALL_THREAD_YIELD
        ) ||
        !ring3_code_emit_syscall(&builder)) {

        return false;
    }

    /*
     * test rax, rax
     *
     * A successful yield must return zero.
     */
    if (!ring3_code_emit8(&builder, 0x48) ||
        !ring3_code_emit8(&builder, 0x85) ||
        !ring3_code_emit8(&builder, 0xC0) ||
        !ring3_code_emit_jne_fail(&builder) ||

        !ring3_code_emit_store_rax_rsp(
            &builder,
            0xE8
        ) ||

        !ring3_code_emit_register_checks(&builder) ||

        /*
         * dec dword [rsp - 48]
         */
        !ring3_code_emit8(&builder, 0xFF) ||
        !ring3_code_emit8(&builder, 0x4C) ||
        !ring3_code_emit8(&builder, 0x24) ||
        !ring3_code_emit8(&builder, 0xD0) ||

        !ring3_code_emit_jne_target(
            &builder,
            yield_loop
        )) {

        return false;
    }

    /*
     * ----------------------------------------------------------------------
     * Unknown syscall numbers
     * ----------------------------------------------------------------------
     *
     * Every unknown number must return -38.
     */

    if (!ring3_code_emit_invalid_eax_syscall(
            &builder,
            HUA_SYSCALL_COUNT
        ) ||

        !ring3_code_emit_invalid_eax_syscall(
            &builder,
            HUA_SYSCALL_COUNT + 1U
        ) ||

        !ring3_code_emit_invalid_eax_syscall(
            &builder,
            0xFFFFFFFFU
        ) ||

        !ring3_code_emit_invalid_rax_syscall(
            &builder,
            UINT64_MAX
        ) ||

        !ring3_code_emit_invalid_rax_syscall(
            &builder,
            0x8000000000000000ULL
        )) {

        return false;
    }

    /*
     * All tests succeeded.
     *
     * status = 0
     */
    if (!ring3_code_emit_mov_dword_rsp(
            &builder,
            0xC0,
            0
        )) {

        return false;
    }

    /*
     * Skip the shared failure block.
     */
    size_t success_jump =
        ring3_code_emit_jmp_placeholder(&builder);

    if (success_jump == SIZE_MAX) {
        return false;
    }

    /*
     * Shared failure block.
     *
     * status = 1
     */
    size_t failure_target = builder.length;

    if (!ring3_code_emit_mov_dword_rsp(
            &builder,
            0xC0,
            1
        )) {

        return false;
    }

    /*
     * Both success and failure reach the same INT3.
     */
    size_t breakpoint_offset = builder.length;

    if (!ring3_code_emit8(&builder, 0xCC)) {
        return false;
    }

    /*
     * Patch every failed comparison to the failure block.
     */
    for (size_t i = 0;
         i < builder.fail_jump_count;
         i++) {

        if (!ring3_code_patch_rel32(
                &builder,
                builder.fail_jumps[i],
                failure_target
            )) {

            return false;
        }
    }

    /*
     * Patch the successful path to skip the failure block.
     */
    if (!ring3_code_patch_rel32(
            &builder,
            success_jump,
            breakpoint_offset
        )) {

        return false;
    }

    if (builder.failed ||
        builder.length > capacity) {

        return false;
    }

    *program_size_out = builder.length;

    *breakpoint_offset_out = breakpoint_offset;

    return true;
}

/*
 * Build a non-returning user termination program.
 *
 * Generated instructions:
 *
 *     mov eax, syscall_number
 *     mov rdi, signed_exit_status
 *     syscall
 *     ud2
 *
 * UD2 must never execute when the requested termination
 * syscall works correctly.
 *
 * The UD2 fallback ensures an unexpected syscall return
 * cannot silently be mistaken for successful termination.
 */
static bool ring3_test_build_exit_program(
    uint8_t *output,
    size_t capacity,
    size_t *program_size_out,
    uint32_t syscall_number,
    int64_t exit_status
) {

    if (output == NULL ||
        program_size_out == NULL ||
        capacity == 0 ||

        (syscall_number != HUA_SYSCALL_THREAD_EXIT &&
         syscall_number != HUA_SYSCALL_PROCESS_EXIT)) {

        return false;
    }

    struct ring3_test_code_builder builder = {
        .bytes = output,
        .capacity = capacity,
        .length = 0,
        .fail_jump_count = 0,
        .failed = false
    };

    if (!ring3_code_emit_mov_eax(
            &builder,
            syscall_number
        ) ||

        !ring3_code_emit_mov_rdi_imm64(
            &builder,
            (uint64_t)exit_status
        ) ||

        !ring3_code_emit_syscall(&builder) ||

        /*
         * UD2: reached only if the termination syscall returns.
         */
        !ring3_code_emit8(&builder, 0x0F) ||

        !ring3_code_emit8(&builder, 0x0B)) {

        return false;
    }

    if (builder.failed ||
        builder.length == 0 ||
        builder.length > capacity) {

        return false;
    }

    *program_size_out = builder.length;

    return true;
}

/*
 * Build the healthy process's Ring 3 program.
 *
 * RBX = read-only control-page user address
 * R13 = writable status-page user address
 * R12D = bounded wait counter
 *
 * The program:
 * 1. Publishes B_STARTED.
 * 2. Repeatedly yields while faulting_reaped == 0.
 * 3. Observes A_REAPED and publishes B_ACKNOWLEDGED.
 * 4. Exits normally.
 *
 * A nonzero unexpected control value, a failed yield,
 * or timeout causes a distinctive failure exit.
 */
static bool ring3_test_build_isolation_survivor_program(
    uint8_t *output,
    size_t capacity,
    size_t *program_size_out
) {

    if (output == NULL ||
        capacity == 0 ||
        program_size_out == NULL) {

        return false;
    }

    *program_size_out = 0;

    struct ring3_test_code_builder builder = {
        .bytes = output,
        .capacity = capacity,
        .length = 0,
        .fail_jump_count = 0,
        .failed = false
    };

    /*
     * mov rbx, CONTROL_VA
     */
    if (!ring3_code_emit8(&builder, 0x48) ||
        !ring3_code_emit8(&builder, 0xBB) ||
        !ring3_code_emit64(
            &builder,
            RING3_TEST_ISOLATION_CONTROL_VA
        )) {

        return false;
    }

    /*
     * mov r13, STATUS_VA
     */
    if (!ring3_code_emit8(&builder, 0x49) ||
        !ring3_code_emit8(&builder, 0xBD) ||
        !ring3_code_emit64(
            &builder,
            RING3_TEST_ISOLATION_STATUS_VA
        )) {

        return false;
    }

    /*
     * mov r12d, 0x00100000
     */
    if (!ring3_code_emit8(&builder, 0x41) ||
        !ring3_code_emit8(&builder, 0xBC) ||
        !ring3_code_emit32(&builder, 0x00100000U)) {

        return false;
    }

    /*
     * Publish B_STARTED:
     *
     * mov dword [r13 + 0], immediate
     *
     * The explicit zero displacement makes the R13-based
     * address encoding unambiguous.
     */
    if (!ring3_code_emit8(&builder, 0x41) ||
        !ring3_code_emit8(&builder, 0xC7) ||
        !ring3_code_emit8(&builder, 0x45) ||
        !ring3_code_emit8(&builder, 0x00) ||
        !ring3_code_emit32(
            &builder,
            RING3_TEST_ISOLATION_B_STARTED
        )) {

        return false;
    }

    size_t loop_offset = builder.length;

    /*
     * Yield.
     */
    if (!ring3_code_emit_mov_eax(
            &builder,
            HUA_SYSCALL_THREAD_YIELD
        ) ||
        !ring3_code_emit_syscall(&builder)) {

        return false;
    }

    /*
     * test rax, rax
     */
    if (!ring3_code_emit8(&builder, 0x48) ||
        !ring3_code_emit8(&builder, 0x85) ||
        !ring3_code_emit8(&builder, 0xC0) ||
        !ring3_code_emit8(&builder, 0x75)) {

        return false;
    }

    size_t yield_failure_displacement = builder.length;

    if (!ring3_code_emit8(&builder, 0)) {
        return false;
    }

    /*
     * cmp dword [rbx], A_REAPED
     */
    if (!ring3_code_emit8(&builder, 0x81) ||
        !ring3_code_emit8(&builder, 0x3B) ||
        !ring3_code_emit32(
            &builder,
            RING3_TEST_ISOLATION_A_REAPED
        ) ||
        !ring3_code_emit8(&builder, 0x74)) {

        return false;
    }

    size_t success_displacement = builder.length;

    if (!ring3_code_emit8(&builder, 0)) {
        return false;
    }

    /*
     * An unexpected nonzero control marker means abort.
     *
     * cmp dword [rbx], 0
     */
    if (!ring3_code_emit8(&builder, 0x83) ||
        !ring3_code_emit8(&builder, 0x3B) ||
        !ring3_code_emit8(&builder, 0x00) ||
        !ring3_code_emit8(&builder, 0x75)) {

        return false;
    }

    size_t abort_displacement = builder.length;

    if (!ring3_code_emit8(&builder, 0)) {
        return false;
    }

    /*
     * dec r12d
     * jnz loop
     */
    if (!ring3_code_emit8(&builder, 0x41) ||
        !ring3_code_emit8(&builder, 0xFF) ||
        !ring3_code_emit8(&builder, 0xCC) ||
        !ring3_code_emit8(&builder, 0x75)) {

        return false;
    }

    size_t loop_displacement = builder.length;

    if (!ring3_code_emit8(&builder, 0)) {
        return false;
    }

    /*
     * Failure block:
     * PROCESS_EXIT(failure_status)
     * UD2 if the syscall unexpectedly returns.
     */
    size_t failure_offset = builder.length;

    if (!ring3_code_emit_mov_eax(
            &builder,
            HUA_SYSCALL_PROCESS_EXIT
        ) ||
        !ring3_code_emit_mov_rdi_imm64(
            &builder,
            (uint64_t)RING3_TEST_ISOLATION_SURVIVOR_FAILURE_STATUS
        ) ||
        !ring3_code_emit_syscall(&builder) ||
        !ring3_code_emit8(&builder, 0x0F) ||
        !ring3_code_emit8(&builder, 0x0B)) {

        return false;
    }

    /*
     * Success block:
     * status->survivor_acknowledged = B_ACKNOWLEDGED
     *
     * mov dword [r13 + 4], immediate
     */
    size_t success_offset = builder.length;

    if (!ring3_code_emit8(&builder, 0x41) ||
        !ring3_code_emit8(&builder, 0xC7) ||
        !ring3_code_emit8(&builder, 0x45) ||
        !ring3_code_emit8(&builder, 0x04) ||
        !ring3_code_emit32(
            &builder,
            RING3_TEST_ISOLATION_B_ACKNOWLEDGED
        )) {

        return false;
    }

    /*
     * Exit normally.
     */
    if (!ring3_code_emit_mov_eax(
            &builder,
            HUA_SYSCALL_PROCESS_EXIT
        ) ||
        !ring3_code_emit_mov_rdi_imm64(
            &builder,
            (uint64_t)RING3_TEST_PROCESS_EXIT_EXPECTED_STATUS
        ) ||
        !ring3_code_emit_syscall(&builder) ||
        !ring3_code_emit8(&builder, 0x0F) ||
        !ring3_code_emit8(&builder, 0x0B)) {

        return false;
    }

    /*
     * Patch all short relative branches.
     */
    const struct {
        size_t displacement_offset;
        size_t target_offset;
    } patches[] = {

        { yield_failure_displacement, failure_offset },
        { success_displacement, success_offset },
        { abort_displacement, failure_offset },
        { loop_displacement, loop_offset }
    };

    for (size_t i = 0;
         i < sizeof(patches) / sizeof(patches[0]);
         i++) {

        int64_t delta =
            (int64_t)patches[i].target_offset -
            (int64_t)(patches[i].displacement_offset + 1U);

        if (delta < -128 || delta > 127) {
            return false;
        }

        builder.bytes[patches[i].displacement_offset] =
            (uint8_t)(int8_t)delta;
    }

    if (builder.failed ||
        builder.length == 0 ||
        builder.length > capacity) {

        return false;
    }

    *program_size_out = builder.length;

    return true;
}

/* -------------------------------------------------------------------------- */
/* Worker context                                                             */
/* -------------------------------------------------------------------------- */

struct ring3_test_worker_context {

    struct process *process;

    vmm_address_space_t *address_space;

    /*
     * Kernel-accessible HHDM alias of the physical user code page.
     *
     * The worker uses this after the returning Ring 3 tests
     * finish to install the one-way thread-exit program.
     */
    uint8_t *user_code_page;

    uintptr_t user_breakpoint_rip;

    uint32_t started;

    uint32_t completed;

    uint32_t success;

    /*
     * Number of times the bootstrap waiting thread
     * has resumed from its own yield operation.
     */
    uint32_t bootstrap_resumptions;

        /*
     * Entry point for one-way exception tests.
     */
    uintptr_t user_entry;
};

/*
 * Kernel-side context for the healthy process in the
 * two-process fault-isolation test.
 *
 * This structure remains on the bootstrap test thread's
 * stack until both user processes have been reaped.
 */
struct ring3_test_isolation_survivor_context {

    struct process *process;

    vmm_address_space_t *address_space;

    uint32_t started;

    uint32_t completed;

    uint32_t success;
};

/*
 * Generate a tiny user-mode program that deliberately
 * triggers one of the supported exceptions.
 *
 * Vector 0:
 *     xor eax, eax
 *     xor edx, edx
 *     xor ecx, ecx
 *     div ecx
 *
 * Vector 6:
 *     ud2
 *
 * Vector 13:
 *     cli
 *
 * Vector 14:
 *     mov rax, unmapped_address
 *     mov rax, [rax]
 *
 * Returns the exact expected faulting RIP and CR2 value.
 */
static bool ring3_test_build_exception_program(
    uint8_t *code_page,
    size_t capacity,
    ring3_exception_test_case_t test_case,
    size_t *program_size_out,
    uintptr_t *faulting_rip_out,
    uint64_t *fault_address_out
) {
    if (code_page == NULL ||
        program_size_out == NULL ||
        faulting_rip_out == NULL ||
        fault_address_out == NULL) {

        return false;
    }

    *program_size_out = 0;
    *faulting_rip_out = 0;
    *fault_address_out = 0;

    /*
     * Divide Error (#DE).
     *
     * 31 C0       xor eax, eax
     * 31 D2       xor edx, edx
     * 31 C9       xor ecx, ecx
     * F7 F1       div ecx
     *
     * The divisor is zero, so DIV raises #DE.
     * The DIV instruction begins at offset 6.
     */
    if (test_case == RING3_EXCEPTION_TEST_DIVIDE_ERROR) {

        if (capacity < 8) {
            return false;
        }

        code_page[0] = 0x31;
        code_page[1] = 0xC0;

        code_page[2] = 0x31;
        code_page[3] = 0xD2;

        code_page[4] = 0x31;
        code_page[5] = 0xC9;

        code_page[6] = 0xF7;
        code_page[7] = 0xF1;

        *program_size_out = 8;
        *faulting_rip_out = RING3_TEST_CODE_VA + 6ULL;

        return true;
    }

    /*
     * Invalid Opcode (#UD).
     *
     * 0F 0B       ud2
     *
     * The processor saves RIP pointing directly
     * at the UD2 instruction.
     */
    if (test_case == RING3_EXCEPTION_TEST_INVALID_OPCODE) {

        if (capacity < 2) {
            return false;
        }

        code_page[0] = 0x0F;
        code_page[1] = 0x0B;

        *program_size_out = 2;
        *faulting_rip_out = RING3_TEST_CODE_VA;

        return true;
    }

    /*
     * General Protection Fault (#GP).
     *
     * FA          cli
     *
     * CLI is privileged when CPL > IOPL.
     * At Ring 3 with IOPL=0, it raises #GP(0).
     */
    if (test_case == RING3_EXCEPTION_TEST_GENERAL_PROTECTION) {

        if (capacity < 1) {
            return false;
        }

        code_page[0] = 0xFA;

        *program_size_out = 1;
        *faulting_rip_out = RING3_TEST_CODE_VA;

        return true;
    }

    /*
     * Page Fault (#PF).
     *
     * 48 B8 imm64     mov rax, unmapped_address
     * 48 8B 00        mov rax, [rax]
     *
     * The second instruction faults because the
     * supplied address is deliberately unmapped.
     */
    if (test_case == RING3_EXCEPTION_TEST_UNMAPPED_READ) {

        if (capacity < 13) {
            return false;
        }

        const uint64_t fault_address =
            RING3_TEST_EXCEPTION_FAULT_VA;

        code_page[0] = 0x48;
        code_page[1] = 0xB8;

        for (uint32_t i = 0; i < 8; i++) {

            code_page[2 + i] =
                (uint8_t)(fault_address >> (i * 8));
        }

        code_page[10] = 0x48;
        code_page[11] = 0x8B;
        code_page[12] = 0x00;

        *program_size_out = 13;

        /*
         * MOV RAX,[RAX] begins ten bytes after
         * the beginning of the code page.
         */
        *faulting_rip_out =
            RING3_TEST_CODE_VA + 10ULL;

        *fault_address_out = fault_address;

        return true;
    }

    /*
    * Read-only write (#PF).
    *
    * 48 B8 imm64     mov rax, RING3_TEST_READONLY_WRITE_VA
    * C6 00 90        mov byte [rax], 0x90
    *
    * The code page is user-accessible, executable,
    * and read-only. The second instruction must fault.
    */
    if (test_case == RING3_EXCEPTION_TEST_READONLY_WRITE) {

        if (capacity < 13) {
            return false;
        }

        const uint64_t target =
            RING3_TEST_READONLY_WRITE_VA;

        code_page[0] = 0x48;
        code_page[1] = 0xB8;

        for (uint32_t i = 0; i < 8; i++) {
            code_page[2 + i] =
                (uint8_t)(target >> (i * 8));
        }

        code_page[10] = 0xC6;
        code_page[11] = 0x00;
        code_page[12] = 0x90;

        *program_size_out = 13;

        *faulting_rip_out =
            RING3_TEST_CODE_VA + 10ULL;

        *fault_address_out = target;

        return true;
    }

    /*
    * NX execution violation (#PF).
    *
    * 48 B8 imm64     mov rax, RING3_TEST_STACK_VA
    * FF E0           jmp rax
    *
    * The stack is user-accessible and writable,
    * but NX. Instruction fetch must fault.
    */
    if (test_case == RING3_EXCEPTION_TEST_NX_EXECUTE) {

        if (capacity < 12) {
            return false;
        }

        const uint64_t target =
            RING3_TEST_STACK_VA;

        code_page[0] = 0x48;
        code_page[1] = 0xB8;

        for (uint32_t i = 0; i < 8; i++) {
            code_page[2 + i] =
                (uint8_t)(target >> (i * 8));
        }

        code_page[10] = 0xFF;
        code_page[11] = 0xE0;

        *program_size_out = 12;

        /*
        * The failed instruction fetch occurs at the
        * destination, not at the preceding JMP.
        */
        *faulting_rip_out = target;

        *fault_address_out = target;

        return true;
    }

    /*
    * Supervisor access violation (#PF).
    *
    * 48 B8 imm64     mov rax, RING3_TEST_SUPERVISOR_VA
    * 48 8B 00        mov rax, [rax]
    *
    * The page is present but supervisor-only.
    * A Ring 3 read must fault.
    */
    if (test_case == RING3_EXCEPTION_TEST_SUPERVISOR_READ) {

        if (capacity < 13) {
            return false;
        }

        const uint64_t target =
            RING3_TEST_SUPERVISOR_VA;

        code_page[0] = 0x48;
        code_page[1] = 0xB8;

        for (uint32_t i = 0; i < 8; i++) {
            code_page[2 + i] =
                (uint8_t)(target >> (i * 8));
        }

        code_page[10] = 0x48;
        code_page[11] = 0x8B;
        code_page[12] = 0x00;

        *program_size_out = 13;

        *faulting_rip_out =
            RING3_TEST_CODE_VA + 10ULL;

        *fault_address_out = target;

        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */


/*
 * Zero a physical frame through its HHDM mapping.
 */
static bool ring3_test_zero_frame(
    uint64_t physical_address
) {

    uint8_t *memory =
        (uint8_t *)pmm_frame_to_virt(physical_address);

    if (memory == NULL) {
        return false;
    }

    for (uint64_t i = 0;
         i < PMM_PAGE_SIZE;
         i++) {

        memory[i] = 0;
    }

    return true;
}


/*
 * Read the active CR3 physical root.
 */
static uint64_t ring3_test_read_cr3(void) {

    uint64_t cr3 = 0;

    asm volatile (
        "mov %%cr3, %0"
        : "=r"(cr3)
        :
        : "memory"
    );

    return cr3 & RING3_TEST_CR3_MASK;
}

/*
 * Wait until the survivor has published its actual Ring 3
 * start marker.
 *
 * The kernel-side worker->started flag is not sufficient:
 * this checks the marker written by generated user code.
 */
static bool ring3_test_isolation_wait_for_survivor_start(
    process_t *survivor,
    struct ring3_test_isolation_status *status,
    struct scheduler_cpu *cpu
) {

    if (survivor == NULL ||
        status == NULL ||
        cpu == NULL) {

        return false;
    }

    uint64_t start_ticks = cpu->timer_ticks;

    volatile uint64_t *tick_counter =
        &cpu->timer_ticks;

    for (uint64_t attempts = 0;
         attempts < 1000000ULL;
         attempts++) {

        uint32_t started =
            __atomic_load_n(
                &status->survivor_started,
                __ATOMIC_ACQUIRE
            );

        if (started == RING3_TEST_ISOLATION_B_STARTED) {
            return true;
        }

        process_state_t state = PROCESS_STATE_NEW;

        uint32_t thread_count = UINT32_MAX;

        if (!process_get_info(
                survivor,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            )) {

            return false;
        }

        /*
         * A process that becomes ZOMBIE before publishing
         * its Ring 3 marker cannot satisfy this test.
         */
        if (state == PROCESS_STATE_ZOMBIE &&
            thread_count == 0) {

            return false;
        }

        if (*tick_counter - start_ticks >=
            RING3_TEST_TIMEOUT_TICKS) {

            return false;
        }

        if (!thread_yield()) {
            return false;
        }
    }

    return false;
}


/*
 * Wait until a process has been fully reaped.
 *
 * ZOMBIE alone is not enough: the process must have no
 * attached threads remaining.
 */
static bool ring3_test_isolation_wait_for_zombie(
    process_t *process,
    struct scheduler_cpu *cpu
) {

    if (process == NULL || cpu == NULL) {
        return false;
    }

    uint64_t start_ticks = cpu->timer_ticks;

    volatile uint64_t *tick_counter =
        &cpu->timer_ticks;

    for (uint64_t attempts = 0;
         attempts < 1000000ULL;
         attempts++) {

        process_state_t state = PROCESS_STATE_NEW;

        uint32_t thread_count = UINT32_MAX;

        if (!process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            )) {

            return false;
        }

        if (state == PROCESS_STATE_ZOMBIE &&
            thread_count == 0) {

            return true;
        }

        if (*tick_counter - start_ticks >=
            RING3_TEST_TIMEOUT_TICKS) {

            return false;
        }

        if (!thread_yield()) {
            return false;
        }
    }

    return false;
}


/*
 * Read the process lifecycle state without yielding.
 */
static bool ring3_test_isolation_process_reaped(
    process_t *process
) {

    if (process == NULL) {
        return false;
    }

    process_state_t state = PROCESS_STATE_NEW;

    uint32_t thread_count = UINT32_MAX;

    return
        process_get_info(
            process,
            NULL,
            &state,
            &thread_count,
            NULL,
            0
        ) &&
        state == PROCESS_STATE_ZOMBIE &&
        thread_count == 0;
}

/*
 * Confirm that HUA's kernel address space is active.
 *
 * User address-space teardown must not occur while that
 * address space is currently loaded in CR3.
 */
static bool ring3_test_kernel_space_active(void) {

    uint64_t expected_root = 0;

    vmm_address_space_t *kernel_space =
        vmm_kernel_address_space();

    if (kernel_space == NULL ||
        !vmm_address_space_get_root_physical(
            kernel_space,
            &expected_root
        )) {

        return false;
    }

    return ring3_test_read_cr3() == expected_root;
}


/*
 * Check an address-space mapping and its effective permissions.
 */
static bool ring3_test_check_mapping(
    vmm_address_space_t *address_space,
    uintptr_t virtual_address,
    uint64_t expected_physical,
    bool expected_writable,
    bool expected_executable
) {

    vmm_mapping_info_t mapping = {0};

    if (vmm_address_space_query(
            address_space,
            virtual_address,
            &mapping
        ) != VMM_STATUS_OK) {

        return false;
    }

    if (mapping.physical_address != expected_physical ||
        mapping.page_size != PMM_PAGE_SIZE ||
        mapping.huge_page ||
        !mapping.user_accessible ||
        mapping.writable != expected_writable ||
        mapping.executable != expected_executable) {

        return false;
    }

    return true;
}

/*
 * User RSP initially points to the top of the stack page.
 *
 * The program writes:
 *
 *     RSP -   8: ABI version
 *     RSP -  16: process ID
 *     RSP -  24: yield result
 *     RSP -  32: invalid syscall result
 *     RSP -  40: ABI loop counter
 *     RSP -  48: yield loop counter
 *     RSP -  56: original RSP
 *     RSP -  64: overall program status
 *
 *     RSP -  72: process-info valid result
 *     RSP -  80: process-info NULL result
 *     RSP -  88: process-info unmapped result
 *     RSP -  96: process-info read-only result
 *
 *     RSP - 112: hua_process_info_t (16 bytes)
 *
 *     RSP - 120: process-info cross-page result
 */

static bool ring3_test_verify_syscall_results(
    struct process *process,
    uint64_t stack_physical
) {

    if (process == NULL ||
        stack_physical == 0) {

        return false;
    }

    uint64_t *stack_words =
        (uint64_t *)pmm_frame_to_virt(stack_physical);

    if (stack_words == NULL) {
        return false;
    }

    size_t word_count =
        PMM_PAGE_SIZE / sizeof(uint64_t);

    if (word_count < 16U) {
        return false;
    }

    /*
     * User program result layout:
     *
     *     RSP -  8: ABI version
     *     RSP - 16: PID
     *     RSP - 24: yield result
     *     RSP - 32: invalid syscall result
     *     RSP - 40: ABI loop counter
     *     RSP - 48: yield loop counter
     *     RSP - 56: original RSP
     *     RSP - 64: status
     */

   /*
    * The ABI-version result was preserved before the
    * cross-page test at RSP - 128.
    */
    uint64_t abi_version =
        stack_words[word_count - 16U];

    /*
    * The original RSP - 8 slot now contains the sentinel.
    */
    uint64_t cross_page_sentinel =
        stack_words[word_count - 1U];

    uint64_t returned_pid =
        stack_words[word_count - 2U];

    uint64_t yield_result =
        stack_words[word_count - 3U];

    uint64_t invalid_result =
        stack_words[word_count - 4U];

    uint64_t abi_loop_remaining =
        stack_words[word_count - 5U];

    uint64_t yield_loop_remaining =
        stack_words[word_count - 6U];

    uint64_t original_user_rsp =
        stack_words[word_count - 7U];

    uint64_t program_status =
        stack_words[word_count - 8U];

    /*
     * Process-info syscall results.
     */
    uint64_t process_info_success =
        stack_words[word_count - 9U];

    uint64_t process_info_null_result =
        stack_words[word_count - 10U];

    uint64_t process_info_unmapped_result =
        stack_words[word_count - 11U];

    uint64_t process_info_readonly_result =
        stack_words[word_count - 12U];

    uint64_t process_info_cross_page_result =
        stack_words[word_count - 15U];

    /*
     * The returned structure starts 112 bytes below
     * the initial user RSP.
     */
    const hua_process_info_t *returned_info =
        (const hua_process_info_t *)(
            (const uint8_t *)stack_words +
            PMM_PAGE_SIZE -
            112U
        );

    /*
     * Get the expected PID from the process manager.
     */
    process_id_t expected_pid = 0;

    if (!process_get_info(
            process,
            &expected_pid,
            NULL,
            NULL,
            NULL,
            0
        ) ||
        expected_pid == 0) {

        console_write(
            "[ FAILED ] Expected process PID retrieval\n"
        );

        return false;
    }

    /*
     * The machine-code program must have reached its
     * success block.
     */
    if (program_status != 0) {

        console_write(
            "[ FAILED ] Ring 3 torture program reported failure\n"
        );

        return false;
    }

    console_write(
        "[ OK ] Ring 3 register and syscall checks succeeded\n"
    );

        /*
     * The valid process-info destination must have succeeded.
     */
    if (process_info_success != 0) {

        console_write(
            "[ FAILED ] GET_PROCESS_INFO valid destination\n"
        );

        return false;
    }

    /*
     * Every invalid destination must return -14.
     */
    if (process_info_null_result !=
            HUA_SYSCALL_ERROR_BAD_ADDRESS ||

        process_info_unmapped_result !=
            HUA_SYSCALL_ERROR_BAD_ADDRESS ||

        process_info_readonly_result !=
            HUA_SYSCALL_ERROR_BAD_ADDRESS ||

        process_info_cross_page_result !=
            HUA_SYSCALL_ERROR_BAD_ADDRESS) {

        console_write(
            "[ FAILED ] GET_PROCESS_INFO invalid destinations\n"
        );

        return false;
    }

    console_write(
        "[ OK ] GET_PROCESS_INFO rejected invalid destinations\n"
    );

    /*
     * Verify the actual structure copied into Ring 3.
     *
     * The user process owns exactly one attached worker
     * thread during this syscall test.
     *
     * Its state must be RUNNING while that worker executes.
     */
    if (returned_info->pid != (uint64_t)expected_pid ||
        returned_info->state != HUA_PROCESS_STATE_RUNNING ||
        returned_info->thread_count != 1U) {

        console_write(
            "[ FAILED ] GET_PROCESS_INFO returned incorrect process data\n"
        );

        return false;
    }

    console_write(
        "[ OK ] GET_PROCESS_INFO returned PID, RUNNING state and thread count\n"
    );

    /*
     * The cross-page test started at RSP - 8.
     *
     * The first eight bytes of that range contained the ABI
     * version result. It must remain intact, proving that
     * the rejected copy did not partially modify the range.
     */

    /*
     * Every ABI-version iteration must have completed.
     */
    if (abi_version != HUA_SYSCALL_ABI_VERSION ||
        cross_page_sentinel != RING3_TEST_CROSS_PAGE_SENTINEL ||
        abi_loop_remaining != 0) {

        console_write(
            "[ FAILED ] ABI-version or cross-page copy integrity test\n"
        );

        return false;
    }

    console_write(
        "[ OK ] ABI version preserved after 128 repeated syscalls\n"
    );

    console_write(
        "[ OK ] Cross-page GET_PROCESS_INFO failure preserved destination\n"
    );

    console_write(
        "[ OK ] 128 repeated ABI-version syscalls\n"
    );

    /*
     * The PID must belong to the calling process.
     */
    if (returned_pid != (uint64_t)expected_pid) {

        console_write(
            "[ FAILED ] Ring 3 syscall process ID\n"
        );

        return false;
    }

    console_write(
        "[ OK ] Calling process PID verified\n"
    );

    /*
     * All 32 yielding syscalls must have returned zero.
     */
    if (yield_result != 0 ||
        yield_loop_remaining != 0) {

        console_write(
            "[ FAILED ] Repeated yielding syscall tests\n"
        );

        return false;
    }

    console_write(
        "[ OK ] 32 repeated yielding syscalls\n"
    );

    /*
     * All unknown syscall-number tests must have returned -38.
     */
    if (invalid_result !=
        HUA_SYSCALL_ERROR_NOT_IMPLEMENTED) {

        console_write(
            "[ FAILED ] Invalid syscall-number tests\n"
        );

        return false;
    }

    console_write(
        "[ OK ] Invalid and extreme syscall numbers rejected\n"
    );

    /*
     * Verify that SYSRETQ restored the original user RSP.
     */
    if (original_user_rsp != RING3_TEST_STACK_TOP) {

        console_write(
            "[ FAILED ] User RSP preservation\n"
        );

        return false;
    }

    console_write(
        "[ OK ] User RSP preserved across repeated syscalls\n"
    );

    return true;
}


/*
 * Unmap a test page and return its physical frame to PMM.
 *
 * If unmapping fails, never free the physical frame.
 * It might still be referenced by the address space.
 *
 * If the returned physical frame does not match the
 * expected frame, do not free either frame blindly.
 */
static bool ring3_test_release_page(
    vmm_address_space_t *address_space,
    uintptr_t virtual_address,
    uint64_t expected_physical,
    bool mapped
) {

    if (expected_physical == 0) {
        return !mapped;
    }

    uint64_t released_physical = expected_physical;

    if (mapped) {

        released_physical = 0;

        if (vmm_address_space_unmap_page(
                address_space,
                virtual_address,
                &released_physical
            ) != VMM_STATUS_OK) {

            return false;
        }

        if (released_physical != expected_physical) {

            /*
             * The mapping was removed, but ownership
             * did not match expectations.
             *
             * Do not free an unexpected frame.
             */
            return false;
        }
    }

    return pmm_free_frame(released_physical);
}

/* -------------------------------------------------------------------------- */
/* Worker-side transition                                                     */
/* -------------------------------------------------------------------------- */

/*
 * Save the worker's interrupt state and disable maskable
 * interrupts.
 *
 * The previous IF state is returned.
 */
static uint64_t ring3_test_irq_save_disable(void) {

    uint64_t rflags = 0;

    asm volatile (
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(rflags)
        :
        : "memory"
    );

    return rflags;
}


/*
 * Restore the original IF state.
 *
 * This test modifies only IF, so it does not need to
 * restore unrelated arithmetic flags.
 */
static void ring3_test_irq_restore(uint64_t saved_rflags) {

    if (saved_rflags & (1ULL << 9)) {

        asm volatile (
            "sti"
            :
            :
            : "memory"
        );

    } else {

        asm volatile (
            "cli"
            :
            :
            : "memory"
        );
    }
}


/*
 * Execute the controlled Ring 3 transition.
 *
 * This runs only inside the process-owned worker.
 */
static bool ring3_test_worker_execute(
    struct ring3_test_worker_context *context
) {

    if (context == NULL ||
        context->process == NULL ||
        context->address_space == NULL) {

        return false;
    }

    /*
     * The scheduler must have selected this process-owned
     * thread and activated its private address space.
     */
    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->process != context->process ||
        current->address_space != context->address_space ||
        current->state != THREAD_STATE_RUNNING) {

        return false;
    }

    /*
    * Run negative tests against the real return-state
    * validator while the user process address space
    * and mappings are available.
    */
    if (!syscall_user_return_validation_run_tests(
            current,
            RING3_TEST_CODE_VA,
            RING3_TEST_STACK_TOP
        )) {

        return false;
    }

    /*
     * Verify the worker's own kernel stack metadata.
     */
    if (!current->stack_owned ||
        current->stack_base == NULL ||
        current->stack_top == 0) {

        return false;
    }

    uintptr_t original_rsp0 = 0;

    uintptr_t transition_stack_top = 0;

    bool transition_stack_selected = false;

    bool test_armed = false;

    bool entered_user = false;

    bool breakpoint_handled = false;

    bool stack_restored = false;

    bool result = false;

    bool scheduler_yield_observed = false;

    /*
     * Disable maskable interrupts before changing the
     * privilege-transition stack.
     *
     * This prevents timer preemption during this controlled
     * test and prevents another ordinary interrupt from
     * observing the temporary TSS.RSP0.
     *
     * NMI, double fault and machine check retain their
     * independent IST stacks.
     */
    uint64_t saved_rflags =
        ring3_test_irq_save_disable();

    /*
     * Verify the scheduler configured TSS.RSP0 using
     * this worker's normal kernel stack.
     */
    if (!gdt_get_kernel_stack(&original_rsp0) ||
        original_rsp0 != current->stack_top) {

        goto restore_interrupt_state;
    }


    /*
    * Select the dedicated Ring 3 test entry stack.
    *
    * This stack must be separate from the worker's suspended
    * C call frames.
    */
    transition_stack_top =
        (uintptr_t)(
            ring3_test_syscall_stack +
            sizeof(ring3_test_syscall_stack)
        );

    if (!gdt_set_kernel_stack(transition_stack_top)) {
        goto restore_interrupt_state;
    }

    transition_stack_selected = true;

    /*
    * SYSCALL does not use TSS.RSP0 automatically.
    * Publish the same trusted stack explicitly for syscall entry.
    */
    if (!arch_syscall_set_kernel_stack(transition_stack_top)) {
        goto restore_worker_stack;
    }

    /*
    * Verify both stack configurations.
    */
    uintptr_t observed_transition_stack = 0;
    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_transition_stack) ||
        observed_transition_stack != transition_stack_top ||
        !arch_syscall_get_kernel_stack(&observed_syscall_stack) ||
        observed_syscall_stack != transition_stack_top) {

        goto restore_worker_stack;
    }

    if (transition_stack_top == original_rsp0 ||
        transition_stack_top == 0) {

        goto restore_worker_stack;
    }


    /*
     * Arm the one-shot Ring 3 breakpoint handler.
     */
    if (!ring3_test_arm(
            current,
            transition_stack_top,
            context->user_breakpoint_rip,
            RING3_TEST_STACK_TOP
        )) {

        goto restore_worker_stack;
    }

    test_armed = true;


    /*
     * Enter CPL3.
     *
     * The user code page contains:
     *
     *     0xCC  INT3
     *
     * The breakpoint handler redirects IRETQ to the
     * Ring 0 assembly return trampoline.
     */
    entered_user = arch_ring3_enter_user(
        RING3_TEST_CODE_VA,
        RING3_TEST_STACK_TOP
    );


    /*
     * At this point we should have returned to the
     * original worker kernel stack.
     */
    breakpoint_handled =
        ring3_test_was_hit();

    /*
    * Capture the voluntary-yield counter after the worker
    * has started and before the controlled user transition.
    */

    uint32_t bootstrap_resumptions =
        __atomic_load_n(
            &context->bootstrap_resumptions,
            __ATOMIC_ACQUIRE
        );

    bool bootstrap_count_ok =
        bootstrap_resumptions >=
        RING3_TEST_MIN_BOOTSTRAP_RESUMPTIONS;

    scheduler_yield_observed = bootstrap_count_ok;

    bool syscall_scratch_clean =
        __atomic_load_n(
            &arch_syscall_user_rsp_scratch,
            __ATOMIC_ACQUIRE
        ) == 0;

    if (test_armed) {

        ring3_test_disarm();

        test_armed = false;
    }


restore_worker_stack:

    /*
     * Restore the worker's normal TSS.RSP0 before
     * allowing ordinary interrupts again.
     *
     * Failure here is fatal because continuing with
     * an unexpected privilege-transition stack could
     * corrupt kernel execution.
     */
    if (transition_stack_selected) {

        if (!gdt_set_kernel_stack(original_rsp0) ||
            !arch_syscall_set_kernel_stack(original_rsp0)) {

            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt" ::: "memory");
            }
        }

        uintptr_t restored_syscall_stack = 0;

        if (!arch_syscall_get_kernel_stack(
                &restored_syscall_stack
            ) ||
            restored_syscall_stack != original_rsp0) {

            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt" ::: "memory");
            }
        }

        uintptr_t restored_rsp0 = 0;

        if (!gdt_get_kernel_stack(&restored_rsp0) ||
            restored_rsp0 != original_rsp0) {

            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt" ::: "memory");
            }
        }

        stack_restored = true;
    }


restore_interrupt_state:

    if (test_armed) {
        ring3_test_disarm();
    }

    ring3_test_irq_restore(saved_rflags);

    result =
        entered_user &&
        breakpoint_handled &&
        stack_restored &&
        scheduler_yield_observed &&
        syscall_scratch_clean;

    return result;
}

/*
 * Replace the user code page with a one-way termination program,
 * configure the dedicated syscall-entry stack, and enter Ring 3.
 *
 * This function never returns.
 *
 * require_breakpoint:
 *   true  for the original worker after its controlled INT3 test.
 *   false for a fresh worker entering Ring 3 for the first time.
 */
static _Noreturn void ring3_test_worker_enter_exit_syscall(
    struct ring3_test_worker_context *context,
    uint32_t syscall_number,
    int64_t exit_status,
    bool require_breakpoint
) {

    if (context == NULL ||
        context->process == NULL ||
        context->address_space == NULL ||
        context->user_code_page == NULL ||

        (syscall_number != HUA_SYSCALL_THREAD_EXIT &&
         syscall_number != HUA_SYSCALL_PROCESS_EXIT)) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->process != context->process ||
        current->address_space != context->address_space ||
        current->state != THREAD_STATE_RUNNING ||
        !current->stack_owned ||
        current->stack_top == 0 ||
        current->preempt_count != 0) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    /*
     * The existing returning Ring 3 test must have completed
     * its controlled breakpoint transition.
     *
     * A fresh process-exit worker has no such breakpoint.
     */
    if (require_breakpoint &&
        !ring3_test_was_hit()) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    /*
     * Install the new user program through the HHDM alias.
     */
    size_t program_size = 0;

    if (!ring3_test_build_exit_program(
            context->user_code_page,
            PMM_PAGE_SIZE,
            &program_size,
            syscall_number,
            exit_status
        ) ||
        program_size == 0 ||
        program_size > PMM_PAGE_SIZE) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    /*
     * Ensure the generated code is visible before entering CPL3.
     *
     * x86 provides coherent instruction/data caches. The memory
     * clobber also prevents compiler reordering across this point.
     */
    asm volatile (
        "mfence"
        :
        :
        : "memory"
    );

    /*
     * This transition is deliberately one-way. Disable ordinary
     * maskable interrupts before changing privilege stacks.
     */
    (void)ring3_test_irq_save_disable();

    uintptr_t original_rsp0 = 0;
    uintptr_t original_syscall_stack = 0;

    /*
     * The scheduler must have configured the worker's ordinary
     * stack before this transition.
     */
    if (!gdt_get_kernel_stack(&original_rsp0) ||
        original_rsp0 != current->stack_top ||

        !arch_syscall_get_kernel_stack(
            &original_syscall_stack
        ) ||

        original_syscall_stack != original_rsp0) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    /*
     * Select the dedicated transition and SYSCALL stack.
     */
    uintptr_t transition_stack_top =
        (uintptr_t)(
            ring3_test_syscall_stack +
            sizeof(ring3_test_syscall_stack)
        );

    if (transition_stack_top == 0 ||
        (transition_stack_top & 0xFULL) != 0) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    /*
     * Keep TSS.RSP0 and the trusted software SYSCALL stack equal.
     */
    if (!gdt_set_kernel_stack(transition_stack_top) ||
        !arch_syscall_set_kernel_stack(transition_stack_top)) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    uintptr_t observed_rsp0 = 0;
    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_rsp0) ||
        observed_rsp0 != transition_stack_top ||

        !arch_syscall_get_kernel_stack(
            &observed_syscall_stack
        ) ||

        observed_syscall_stack != transition_stack_top) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    /*
     * Enter Ring 3 without recording a kernel return continuation.
     *
     * A successful termination syscall switches permanently
     * to another thread.
     */
    arch_ring3_enter_user_noreturn(
        RING3_TEST_CODE_VA,
        RING3_TEST_STACK_TOP
    );

    __builtin_unreachable();
}

/*
 * Process-owned Ring 3 test worker.
 *
 * First, execute the returning Ring 3 and syscall tests.
 *
 * Then publish their result to the bootstrap thread.
 *
 * Finally, enter user mode one last time and terminate through
 * HUA_SYSCALL_THREAD_EXIT.
 *
 * This function never completes through a normal C return.
 */
static void ring3_test_worker(void *argument) {

    struct ring3_test_worker_context *context =
        (struct ring3_test_worker_context *)argument;

    if (context == NULL) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    __atomic_store_n(
        &context->started,
        1,
        __ATOMIC_RELEASE
    );

    bool success =
        ring3_test_worker_execute(context);

    /*
     * Publish the ordinary Ring 3 test outcome before the
     * final non-returning transition.
     *
     * The bootstrap thread uses completed together with the
     * process ZOMBIE state to detect termination.
     */
    __atomic_store_n(
        &context->success,
        success ? 1U : 0U,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &context->completed,
        1,
        __ATOMIC_RELEASE
    );

    /*
     * A failed earlier test must not accidentally report the
     * expected THREAD_EXIT status.
     */
    if (!success) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    /*
     * This call is non-returning.
     *
     * The process remains RUNNING until the reaper removes
     * this final thread. process_thread_exit() then makes it
     * ZOMBIE and records its final exit status.
     */
    ring3_test_worker_enter_exit_syscall(
        context,
        HUA_SYSCALL_THREAD_EXIT,
        RING3_TEST_THREAD_EXIT_EXPECTED_STATUS,
        true
    );

    __builtin_unreachable();
}

/*
 * Process-owned worker used exclusively for the PROCESS_EXIT test.
 *
 * It publishes its setup result and then enters Ring 3 permanently.
 * A successful syscall must terminate the entire single-threaded
 * test process.
 */
static void ring3_test_process_exit_worker(void *argument) {

    struct ring3_test_worker_context *context =
        (struct ring3_test_worker_context *)argument;

    if (context == NULL) {

        thread_exit(
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS
        );
    }

    __atomic_store_n(
        &context->started,
        1,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &context->success,
        1,
        __ATOMIC_RELEASE
    );

    /*
     * The bootstrap thread can now wait for process reaping.
     *
     * completed means the worker has reached its final
     * one-way transition; it does not itself mean the
     * process has become a zombie.
     */
    __atomic_store_n(
        &context->completed,
        1,
        __ATOMIC_RELEASE
    );

    ring3_test_worker_enter_exit_syscall(
        context,
        HUA_SYSCALL_PROCESS_EXIT,
        RING3_TEST_PROCESS_EXIT_EXPECTED_STATUS,
        false
    );

    __builtin_unreachable();
}

/*
 * Fail the exception-test worker without pretending that
 * a user-mode exception occurred.
 */
static _Noreturn void ring3_test_exception_worker_fail(
    struct ring3_test_worker_context *context
) {
    if (context != NULL) {

        __atomic_store_n(
            &context->started,
            1,
            __ATOMIC_RELEASE
        );

        __atomic_store_n(
            &context->success,
            0,
            __ATOMIC_RELEASE
        );

        __atomic_store_n(
            &context->completed,
            1,
            __ATOMIC_RELEASE
        );
    }

    thread_exit(
        RING3_TEST_EXCEPTION_FAILURE_STATUS
    );
}


/*
 * Process-owned worker for genuine Ring 3 exception tests.
 *
 * This worker does not execute a syscall to terminate.
 * It enters user mode and deliberately faults.
 *
 * The exception dispatcher must redirect execution
 * through the real user-exception termination path.
 */
static void ring3_test_user_exception_worker(void *argument) {

    struct ring3_test_worker_context *context =
        (struct ring3_test_worker_context *)argument;

    if (context == NULL ||
        context->process == NULL ||
        context->address_space == NULL ||
        context->user_code_page == NULL ||
        context->user_entry != RING3_TEST_CODE_VA) {

        ring3_test_exception_worker_fail(context);
    }

    __atomic_store_n(
        &context->started,
        1,
        __ATOMIC_RELEASE
    );

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->process != context->process ||
        current->address_space != context->address_space ||
        current->state != THREAD_STATE_RUNNING ||
        !current->stack_owned ||
        current->stack_top == 0 ||
        current->preempt_count != 0) {

        ring3_test_exception_worker_fail(context);
    }

    /*
     * The faulting test uses the same dedicated
     * transition-stack arrangement as the existing
     * one-way Ring 3 syscall tests.
     */
    (void)ring3_test_irq_save_disable();

    uintptr_t original_rsp0 = 0;
    uintptr_t original_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&original_rsp0) ||
        original_rsp0 != current->stack_top ||

        !arch_syscall_get_kernel_stack(
            &original_syscall_stack
        ) ||

        original_syscall_stack != original_rsp0) {

        ring3_test_exception_worker_fail(context);
    }

    uintptr_t transition_stack_top =
        (uintptr_t)(
            ring3_test_syscall_stack +
            sizeof(ring3_test_syscall_stack)
        );

    if (transition_stack_top == 0 ||
        (transition_stack_top & 0xFULL) != 0) {

        ring3_test_exception_worker_fail(context);
    }

    /*
     * For #UD, this becomes the privilege-transition stack.
     *
     * For #PF, the IDT selects IST4 instead. Both paths
     * eventually use the explicitly rewritten IRETQ frame
     * to enter the faulting thread's owned kernel stack.
     */
    if (!gdt_set_kernel_stack(transition_stack_top) ||
        !arch_syscall_set_kernel_stack(transition_stack_top)) {

        ring3_test_exception_worker_fail(context);
    }

    uintptr_t observed_rsp0 = 0;
    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_rsp0) ||
        observed_rsp0 != transition_stack_top ||

        !arch_syscall_get_kernel_stack(
            &observed_syscall_stack
        ) ||

        observed_syscall_stack != transition_stack_top) {

        ring3_test_exception_worker_fail(context);
    }

    /*
     * Publish that setup completed and the one-way
     * user-mode transition is about to begin.
     *
     * This is not proof of a successful exception.
     * The parent separately verifies the exception
     * record, process state, and exit status.
     */
    __atomic_store_n(
        &context->success,
        1,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &context->completed,
        1,
        __ATOMIC_RELEASE
    );

    arch_ring3_enter_user_noreturn(
        context->user_entry,
        RING3_TEST_STACK_TOP
    );

    __builtin_unreachable();
}

/*
 * Terminate the survivor worker after a kernel-side setup error.
 *
 * This is deliberately distinguishable from the expected
 * PROCESS_EXIT success status.
 */
static _Noreturn void ring3_test_isolation_survivor_fail(
    struct ring3_test_isolation_survivor_context *context
) {

    if (context != NULL) {

        __atomic_store_n(
            &context->started,
            1,
            __ATOMIC_RELEASE
        );

        __atomic_store_n(
            &context->success,
            0,
            __ATOMIC_RELEASE
        );

        __atomic_store_n(
            &context->completed,
            1,
            __ATOMIC_RELEASE
        );
    }

    thread_exit(
        RING3_TEST_ISOLATION_SURVIVOR_FAILURE_STATUS
    );

    __builtin_unreachable();
}


/*
 * Process-owned worker for the healthy process.
 *
 * Unlike the controlled exception worker, this process
 * enters Ring 3 with interrupts enabled.
 *
 * The scheduler's ordinary per-thread kernel stack is used
 * for privilege transitions and SYSCALL entry.
 *
 * The user program then repeatedly invokes THREAD_YIELD
 * while waiting for the kernel to publish the faulting
 * process's completion marker.
 */
static void ring3_test_isolation_survivor_worker(
    void *argument
) {

    struct ring3_test_isolation_survivor_context *context =
        (struct ring3_test_isolation_survivor_context *)argument;

    if (context == NULL ||
        context->process == NULL ||
        context->address_space == NULL) {

        ring3_test_isolation_survivor_fail(context);
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->process != context->process ||
        current->address_space != context->address_space ||
        current->state != THREAD_STATE_RUNNING ||
        !current->stack_owned ||
        current->stack_base == NULL ||
        current->stack_top == 0 ||
        (current->stack_top & 0xFULL) != 0 ||
        current->preempt_count != 0) {

        ring3_test_isolation_survivor_fail(context);
    }

    /*
     * The normal preemptible Ring 3 entry path must use
     * the kernel stack selected by the scheduler.
     */
    uintptr_t observed_rsp0 = 0;

    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_rsp0) ||
        observed_rsp0 != current->stack_top ||

        !arch_syscall_get_kernel_stack(
            &observed_syscall_stack
        ) ||

        observed_syscall_stack != current->stack_top) {

        ring3_test_isolation_survivor_fail(context);
    }

    /*
     * Publish successful kernel-side setup.
     *
     * This does NOT mean that CPL3 execution has begun.
     * The user program independently publishes B_STARTED
     * into the shared physical coordination page.
     */
    __atomic_store_n(
        &context->success,
        1,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &context->completed,
        1,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &context->started,
        1,
        __ATOMIC_RELEASE
    );

    /*
     * Permanently enter CPL3 with RFLAGS.IF set.
     *
     * The scheduler has already activated this process's
     * address space and configured its kernel transition
     * and SYSCALL stacks.
     */
    arch_ring3_enter_user_preemptible_noreturn(
        RING3_TEST_CODE_VA,
        RING3_TEST_STACK_TOP
    );

    __builtin_unreachable();
}

/* -------------------------------------------------------------------------- */
/* Process cleanup                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Release the test's mappings and process resources.
 *
 * This helper is called only after:
 *
 *     - No worker is running.
 *     - The kernel address space is active.
 *     - The process has no attached threads.
 */
static bool ring3_test_cleanup(
    struct process *process,
    vmm_address_space_t *address_space,

    uint64_t code_physical,
    bool code_mapped,

    uint64_t stack_physical,
    bool stack_mapped
) {

    bool success = true;


    /*
     * Never unmap or destroy an address space that is
     * currently active in CR3.
     */
    if (!ring3_test_kernel_space_active()) {

        console_write(
            "[ RING3 ] Cleanup refused: kernel CR3 is not active\n"
        );

        return false;
    }


    /*
     * Release code page.
     */
    if (!ring3_test_release_page(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            code_mapped
        )) {

        console_write(
            "[ RING3 ] Code page release failed\n"
        );

        success = false;
    }


    /*
     * Release user stack page.
     */
    if (!ring3_test_release_page(
            address_space,
            RING3_TEST_STACK_VA,
            stack_physical,
            stack_mapped
        )) {

        console_write(
            "[ RING3 ] Stack page release failed\n"
        );

        success = false;
    }


    if (process != NULL) {

        process_state_t state = PROCESS_STATE_NEW;

        uint32_t thread_count = 0;


        if (!process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            )) {

            success = false;

        } else {

            /*
             * Recover safely if a failure path left a
             * registered RUNNING process with zero threads.
             */
            if (state == PROCESS_STATE_RUNNING &&
                thread_count == 0) {

                if (!process_begin_exit(process)) {
                    success = false;
                }

                if (!process_get_info(
                        process,
                        NULL,
                        &state,
                        &thread_count,
                        NULL,
                        0
                    )) {

                    success = false;
                }
            }


            /*
             * Destruction requires NEW or ZOMBIE state
             * and zero attached threads.
             */
            if (thread_count == 0 &&
                (state == PROCESS_STATE_NEW ||
                 state == PROCESS_STATE_ZOMBIE)) {

                if (!process_destroy(process)) {

                    console_write(
                        "[ RING3 ] Process destruction failed\n"
                    );

                    success = false;
                }

            } else {

                console_write(
                    "[ RING3 ] Process still has live threads\n"
                );

                success = false;
            }
        }


        /*
         * Release the creator's process reference.
         *
         * If process_destroy failed, the registry still
         * retains its own reference, so this does not
         * create a dangling registry entry.
         */
        process_put(process);
    }

    return success;
}

/* -------------------------------------------------------------------------- */
/* Wait for process termination and reaping                                   */
/* -------------------------------------------------------------------------- */

/*
 * Wait until:
 *
 *     1. The worker completed its test.
 *     2. The worker thread exited.
 *     3. The reaper detached the thread.
 *     4. The process became ZOMBIE.
 *     5. The process thread count reached zero.
 *
 * We repeatedly yield because timer preemption can return
 * to the bootstrap thread before the worker finishes.
 */
static bool ring3_test_wait_for_worker(
    struct process *process,
    struct ring3_test_worker_context *context,
    struct scheduler_cpu *cpu
) {

    if (process == NULL ||
        context == NULL ||
        cpu == NULL) {

        return false;
    }

    uint64_t start_ticks = cpu->timer_ticks;

    volatile uint64_t *tick_counter =
        &cpu->timer_ticks;


    for (uint64_t attempts = 0;
         attempts < 1000000ULL;
         attempts++) {

        uint32_t completed =
            __atomic_load_n(
                &context->completed,
                __ATOMIC_ACQUIRE
            );

        process_state_t state = PROCESS_STATE_NEW;

        uint32_t thread_count = UINT32_MAX;

        bool information_valid =
            process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            );

        if (completed == 1 &&
            information_valid &&
            state == PROCESS_STATE_ZOMBIE &&
            thread_count == 0) {

            return true;
        }


        uint64_t current_ticks = *tick_counter;

        if (current_ticks - start_ticks >=
            RING3_TEST_TIMEOUT_TICKS) {

            return false;
        }

        /*
        * When the worker yields from Ring 3, the bootstrap
        * thread is runnable and can resume here.
        *
        * Count these resumptions so the worker can verify
        * that its repeated yielding test gave the bootstrap
        * thread opportunities to run.
        */
        if (thread_yield()) {

            __atomic_fetch_add(
                &context->bootstrap_resumptions,
                1U,
                __ATOMIC_RELEASE
            );

        } else {

            asm volatile ("pause");
        }
    }

    return false;
}

/*
 * Execute PROCESS_EXIT from a genuine Ring 3 context.
 *
 * This creates a separate user process so the existing
 * THREAD_EXIT test remains independent and unchanged.
 */
static bool ring3_test_run_process_exit_syscall(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap,
    uintptr_t bootstrap_rsp0
) {

    if (cpu == NULL ||
        bootstrap == NULL ||
        bootstrap_rsp0 == 0 ||
        scheduler_get_current_thread(0) != bootstrap ||
        !ring3_test_kernel_space_active()) {

        return false;
    }

    console_write(
        "\n========== PROCESS_EXIT SYSCALL TEST ==========\n"
    );

    process_t *process =
        process_create_user("ring3-process-exit-test");

    if (process == NULL) {
        console_write(
            "[ FAILED ] PROCESS_EXIT test process creation\n"
        );
        return false;
    }

    vmm_address_space_t *address_space = NULL;

    uint64_t code_physical = 0;
    uint64_t stack_physical = 0;

    bool code_mapped = false;
    bool stack_mapped = false;

    struct thread *worker = NULL;

    bool worker_enqueued = false;
    bool success = false;

    /*
     * Obtain the process-owned address space.
     */
    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        console_write(
            "[ FAILED ] PROCESS_EXIT address-space retrieval\n"
        );

        goto cleanup;
    }

    /*
     * Allocate independent user code and stack frames.
     */
    code_physical = pmm_alloc_frame();

    if (code_physical == 0) {
        console_write(
            "[ FAILED ] PROCESS_EXIT code frame allocation\n"
        );
        goto cleanup;
    }

    stack_physical = pmm_alloc_frame();

    if (stack_physical == 0) {
        console_write(
            "[ FAILED ] PROCESS_EXIT stack frame allocation\n"
        );
        goto cleanup;
    }

    if (!ring3_test_zero_frame(code_physical) ||
        !ring3_test_zero_frame(stack_physical)) {

        console_write(
            "[ FAILED ] PROCESS_EXIT user frame initialization\n"
        );

        goto cleanup;
    }

    /*
     * Retrieve the kernel-accessible code-frame alias.
     */
    uint8_t *code_page =
        (uint8_t *)pmm_frame_to_virt(code_physical);

    if (code_page == NULL) {
        console_write(
            "[ FAILED ] PROCESS_EXIT code HHDM mapping\n"
        );
        goto cleanup;
    }

    /*
     * Generate:
     *
     *     mov eax, HUA_SYSCALL_PROCESS_EXIT
     *     mov rdi, expected exit status
     *     syscall
     *     ud2
     */
    size_t program_size = 0;

    if (!ring3_test_build_exit_program(
            code_page,
            PMM_PAGE_SIZE,
            &program_size,
            HUA_SYSCALL_PROCESS_EXIT,
            RING3_TEST_PROCESS_EXIT_EXPECTED_STATUS
        ) ||
        program_size == 0 ||
        program_size > PMM_PAGE_SIZE) {

        console_write(
            "[ FAILED ] PROCESS_EXIT program generation\n"
        );

        goto cleanup;
    }

    /*
     * Map the user code as readable and executable,
     * but not writable.
     */
    if (vmm_address_space_map_page(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            VMM_MAP_USER | VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] PROCESS_EXIT user code mapping\n"
        );

        goto cleanup;
    }

    code_mapped = true;

    /*
     * Map the user stack as readable and writable,
     * but non-executable.
     */
    if (vmm_address_space_map_page(
            address_space,
            RING3_TEST_STACK_VA,
            stack_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] PROCESS_EXIT user stack mapping\n"
        );

        goto cleanup;
    }

    stack_mapped = true;

    /*
     * Verify the actual effective page permissions.
     */
    if (!ring3_test_check_mapping(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            false,
            true
        ) ||

        !ring3_test_check_mapping(
            address_space,
            RING3_TEST_STACK_VA,
            stack_physical,
            true,
            false
        )) {

        console_write(
            "[ FAILED ] PROCESS_EXIT code/stack permissions\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] PROCESS_EXIT user mappings verified\n"
    );

    /*
     * The stack is separate from the code page.
     */
    struct ring3_test_worker_context worker_context = {
        .process = process,

        .address_space = address_space,

        .user_code_page = code_page,

        .user_breakpoint_rip = 0,

        .started = 0,

        .completed = 0,

        .success = 0,

        .bootstrap_resumptions = 0
    };

    /*
     * Create exactly one process-owned thread.
     *
     * Attaching it transitions the process to RUNNING.
     */
    worker = thread_create_for_process(
        process,
        "ring3-process-exit-worker",
        ring3_test_process_exit_worker,
        &worker_context,
        0
    );

    if (worker == NULL) {

        console_write(
            "[ FAILED ] PROCESS_EXIT worker creation\n"
        );

        goto cleanup;
    }

    /*
     * Make the worker runnable.
     */
    if (!scheduler_enqueue_thread(0, worker)) {

        if (worker->state != THREAD_STATE_NEW ||
            worker->run_queue_owner != NULL) {

            console_write(
                "[ FATAL ] PROCESS_EXIT enqueue left unexpected TCB state\n"
            );

            return false;
        }

        (void)thread_destroy(worker);
        worker = NULL;

        console_write(
            "[ FAILED ] PROCESS_EXIT worker enqueue\n"
        );

        goto cleanup;
    }

    worker_enqueued = true;

    /*
     * Do not dereference the worker TCB after enqueue.
     * It may execute and be reclaimed before this function resumes.
     */
    worker = NULL;

    /*
     * Wait for both the worker's completion publication
     * and the process's final ZOMBIE transition.
     */
    if (!ring3_test_wait_for_worker(
            process,
            &worker_context,
            cpu
        )) {

        console_write(
            "[ FAILED ] PROCESS_EXIT completion/reaper timeout\n"
        );

        /*
         * The worker may still be executing.
         * Never unmap its code or stack in this state.
         */
        return false;
    }

    console_write(
        "[ OK ] PROCESS_EXIT worker was terminated and reaped\n"
    );

    /*
     * Verify the worker reached its one-way execution path.
     */
    if (__atomic_load_n(
            &worker_context.started,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &worker_context.completed,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &worker_context.success,
            __ATOMIC_ACQUIRE
        ) != 1) {

        console_write(
            "[ FAILED ] PROCESS_EXIT worker execution state\n"
        );

        goto cleanup;
    }

    /*
     * Verify the final signed 64-bit process exit status.
     *
     * The final TCB has already been detached and reclaimed.
     */

    int64_t observed_exit_status = 0;

    /*
    * First determine whether the process manager considers
    * the exit status available.
    */
    if (!process_get_exit_code(
            process,
            &observed_exit_status
        )) {

        console_write(
            "[ FAILED ] PROCESS_EXIT status retrieval rejected\n"
        );

        goto cleanup;
    }

    /*
    * Determine whether the expected user-supplied status
    * survived the complete termination and reaping path.
    */
    if (observed_exit_status !=
        RING3_TEST_PROCESS_EXIT_EXPECTED_STATUS) {

        if (observed_exit_status ==
            RING3_TEST_THREAD_EXIT_FAILURE_STATUS) {

            console_write(
                "[ FAILED ] PROCESS_EXIT worker used a kernel-side failure fallback\n"
            );

        } else {

            console_write(
                "[ FAILED ] PROCESS_EXIT exit status value mismatch\n"
            );
        }

        goto cleanup;
    }

    console_write(
        "[ OK ] PROCESS_EXIT preserved signed 64-bit exit status\n"
    );

    /*
     * Check process lifecycle accounting independently.
     */
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
        state != PROCESS_STATE_ZOMBIE ||
        thread_count != 0) {

        console_write(
            "[ FAILED ] PROCESS_EXIT zombie/thread accounting\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] PROCESS_EXIT reached ZOMBIE with zero threads\n"
    );

    /*
     * The worker must have switched back to the bootstrap
     * thread and its permanent kernel address space.
     */
    if (scheduler_get_current_thread(0) != bootstrap ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        !ring3_test_kernel_space_active()) {

        console_write(
            "[ FAILED ] PROCESS_EXIT bootstrap/kernel restoration\n"
        );

        goto cleanup;
    }

    /*
     * TSS.RSP0 must also belong to the bootstrap thread again.
     */
    uintptr_t observed_rsp0 = 0;

    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_rsp0) ||
        observed_rsp0 != bootstrap_rsp0 ||

        !arch_syscall_get_kernel_stack(
            &observed_syscall_stack
        ) ||

        observed_syscall_stack != bootstrap_rsp0) {

        console_write(
            "[ FAILED ] PROCESS_EXIT privilege-stack restoration\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] PROCESS_EXIT restored bootstrap architecture state\n"
    );

    /*
     * Verify that the assembly syscall entry cleared its
     * temporary user-RSP scratch value.
     */
    if (__atomic_load_n(
            &arch_syscall_user_rsp_scratch,
            __ATOMIC_ACQUIRE
        ) != 0) {

        console_write(
            "[ FAILED ] PROCESS_EXIT syscall scratch was not cleared\n"
        );

        goto cleanup;
    }

    success = true;

cleanup:

    /*
     * Do not clean up resources belonging to a possibly
     * running worker.
     */
    if (worker_enqueued &&
        !ring3_test_kernel_space_active()) {

        console_write(
            "[ FATAL ] PROCESS_EXIT cleanup refused while user CR3 is active\n"
        );

        return false;
    }

    /*
     * If a worker was created but never enqueued, destroy
     * its NEW TCB. This safely removes its process attachment.
     */
    if (worker != NULL) {

        if (worker->state == THREAD_STATE_NEW &&
            worker->run_queue_owner == NULL) {

            if (!thread_destroy(worker)) {
                success = false;
            }

        } else {

            console_write(
                "[ FATAL ] PROCESS_EXIT cleanup found a non-NEW worker\n"
            );

            return false;
        }

        worker = NULL;
    }

    /*
     * Release mappings, frames, process registration,
     * and the creator's process reference.
     */
    bool cleanup_success =
        ring3_test_cleanup(
            process,
            address_space,

            code_physical,
            code_mapped,

            stack_physical,
            stack_mapped
        );

    if (!cleanup_success) {
        success = false;
    }

    if (success) {
        console_write(
            "[ OK ] PROCESS_EXIT resources and address space released\n"
        );

        console_write(
            "========== PROCESS_EXIT TEST PASSED ==========\n"
        );
    }

    return success;
}

/*
 * Execute one genuine Ring 3 exception in an isolated
 * single-threaded user process.
 *
 * Supported:
 *     6  - Invalid Opcode (#UD)
 *     14 - Page Fault (#PF)
 */
static bool ring3_test_run_user_exception(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap,
    uintptr_t bootstrap_rsp0,
    ring3_exception_test_case_t test_case
) {
    if (cpu == NULL ||
        bootstrap == NULL ||
        bootstrap_rsp0 == 0 ||
        (unsigned)test_case > (unsigned)RING3_EXCEPTION_TEST_SUPERVISOR_READ ||
        scheduler_get_current_thread(0) != bootstrap ||
        !ring3_test_kernel_space_active()) {

        return false;
    }

    const char *test_label = NULL;
    const char *process_name = NULL;
    const char *worker_name = NULL;

    uint64_t expected_vector = 0;
    uint64_t expected_page_fault_error = 0;

    switch (test_case) {

        case RING3_EXCEPTION_TEST_DIVIDE_ERROR:
            test_label = "RING 3 DIVIDE ERROR TEST";
            process_name = "ring3-user-de-test";
            worker_name = "ring3-user-de-worker";
            expected_vector = 0;
            break;

        case RING3_EXCEPTION_TEST_INVALID_OPCODE:
            test_label = "RING 3 INVALID OPCODE TEST";
            process_name = "ring3-user-ud-test";
            worker_name = "ring3-user-ud-worker";
            expected_vector = 6;
            break;

        case RING3_EXCEPTION_TEST_GENERAL_PROTECTION:
            test_label = "RING 3 GENERAL PROTECTION TEST";
            process_name = "ring3-user-gp-test";
            worker_name = "ring3-user-gp-worker";
            expected_vector = 13;
            break;

        case RING3_EXCEPTION_TEST_UNMAPPED_READ:
            test_label = "RING 3 UNMAPPED READ TEST";
            process_name = "ring3-user-unmapped-test";
            worker_name = "ring3-user-unmapped-worker";
            expected_vector = 14;
            expected_page_fault_error = 0x04;
            break;

        case RING3_EXCEPTION_TEST_READONLY_WRITE:
            test_label = "RING 3 READ-ONLY WRITE TEST";
            process_name = "ring3-user-readonly-test";
            worker_name = "ring3-user-readonly-worker";
            expected_vector = 14;
            expected_page_fault_error = 0x07;
            break;

        case RING3_EXCEPTION_TEST_NX_EXECUTE:
            test_label = "RING 3 NX EXECUTION TEST";
            process_name = "ring3-user-nx-test";
            worker_name = "ring3-user-nx-worker";
            expected_vector = 14;
            expected_page_fault_error = 0x15;
            break;

        case RING3_EXCEPTION_TEST_SUPERVISOR_READ:
            test_label = "RING 3 SUPERVISOR ACCESS TEST";
            process_name = "ring3-user-supervisor-test";
            worker_name = "ring3-user-supervisor-worker";
            expected_vector = 14;
            expected_page_fault_error = 0x05;
            break;

        default:
            return false;
    }

    console_write("\n========== ");
    console_write(test_label);
    console_write(" ==========\n");

    process_t *process =
        process_create_user(process_name);

    if (process == NULL) {

        console_write(
            "[ FAILED ] Exception-test process creation\n"
        );

        return false;
    }

    vmm_address_space_t *address_space = NULL;

    uint64_t code_physical = 0;
    uint64_t stack_physical = 0;
    uint64_t supervisor_physical = 0;

    bool code_mapped = false;
    bool stack_mapped = false;
    bool supervisor_mapped = false;

    struct thread *worker = NULL;

    bool worker_enqueued = false;
    bool success = false;

    /*
     * Obtain the process-owned address space.
     */
    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        console_write(
            "[ FAILED ] Exception-test address-space retrieval\n"
        );

        goto cleanup;
    }

    /*
     * Allocate independent physical frames for user code
     * and user stack.
     */
    code_physical = pmm_alloc_frame();

    if (code_physical == 0) {

        console_write(
            "[ FAILED ] Exception-test code-frame allocation\n"
        );

        goto cleanup;
    }

    stack_physical = pmm_alloc_frame();

    if (stack_physical == 0) {

        console_write(
            "[ FAILED ] Exception-test stack-frame allocation\n"
        );

        goto cleanup;
    }

    if (!ring3_test_zero_frame(code_physical) ||
        !ring3_test_zero_frame(stack_physical)) {

        console_write(
            "[ FAILED ] Exception-test frame initialization\n"
        );

        goto cleanup;
    }

    uint8_t *code_page =
        (uint8_t *)pmm_frame_to_virt(code_physical);

    if (code_page == NULL) {

        console_write(
            "[ FAILED ] Exception-test code HHDM mapping\n"
        );

        goto cleanup;
    }

    /*
     * Generate exactly one faulting user program.
     */
    size_t program_size = 0;
    uintptr_t expected_faulting_rip = 0;
    uint64_t expected_fault_address = 0;

    if (!ring3_test_build_exception_program(
            code_page,
            PMM_PAGE_SIZE,
            test_case,
            &program_size,
            &expected_faulting_rip,
            &expected_fault_address
        ) ||
        program_size == 0 ||
        program_size > PMM_PAGE_SIZE ||
        expected_faulting_rip == 0) {

        console_write(
            "[ FAILED ] Exception-test program generation\n"
        );

        goto cleanup;
    }

    /*
     * Ensure generated instructions are visible before
     * the processor fetches them from the user mapping.
     */
    asm volatile (
        "mfence"
        :
        :
        : "memory"
    );

    /*
     * Map user code RX, never writable.
     */
    if (vmm_address_space_map_page(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            VMM_MAP_USER | VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Exception-test code mapping\n"
        );

        goto cleanup;
    }

    code_mapped = true;

    /*
     * Map user stack RW and NX.
     */
    if (vmm_address_space_map_page(
            address_space,
            RING3_TEST_STACK_VA,
            stack_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Exception-test stack mapping\n"
        );

        goto cleanup;
    }

    stack_mapped = true;

    /*
     * The supervisor-access test needs one additional page.
     *
     * VMM_MAP_NONE deliberately creates a mapping that is:
     * - Present
     * - Read-only
     * - Supervisor-only
     * - Non-executable
     *
     * All other tests leave this mapping absent.
     */
    if (test_case == RING3_EXCEPTION_TEST_SUPERVISOR_READ) {

        supervisor_physical = pmm_alloc_frame();

        if (supervisor_physical == 0) {

            console_write(
                "[ FAILED ] Supervisor test frame allocation\n"
            );

            goto cleanup;
        }

        if (!ring3_test_zero_frame(supervisor_physical)) {

            console_write(
                "[ FAILED ] Supervisor test frame initialization\n"
            );

            goto cleanup;
        }

        if (vmm_address_space_map_page(
                address_space,
                RING3_TEST_SUPERVISOR_VA,
                supervisor_physical,
                VMM_MAP_NONE
            ) != VMM_STATUS_OK) {

            console_write(
                "[ FAILED ] Supervisor-only page mapping\n"
            );

            goto cleanup;
        }

        supervisor_mapped = true;

        /*
         * Verify the actual effective mapping.
         *
         * Do not use ring3_test_check_mapping(): it requires
         * user_accessible == true.
         */
        vmm_mapping_info_t supervisor_mapping = {0};

        if (vmm_address_space_query(
                address_space,
                RING3_TEST_SUPERVISOR_VA,
                &supervisor_mapping
            ) != VMM_STATUS_OK ||
            supervisor_mapping.physical_address != supervisor_physical ||
            supervisor_mapping.page_size != PMM_PAGE_SIZE ||
            supervisor_mapping.huge_page ||
            supervisor_mapping.user_accessible ||
            supervisor_mapping.writable ||
            supervisor_mapping.executable) {

            console_write(
                "[ FAILED ] Supervisor-only mapping permissions\n"
            );

            goto cleanup;
        }

        console_write(
            "[ OK ] Present supervisor-only RO/NX mapping verified\n"
        );
    }

    /*
     * Verify the effective user permissions.
     */
    if (!ring3_test_check_mapping(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            false,
            true
        ) ||

        !ring3_test_check_mapping(
            address_space,
            RING3_TEST_STACK_VA,
            stack_physical,
            true,
            false
        )) {

        console_write(
            "[ FAILED ] Exception-test mapping permissions\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Exception-test user mappings verified\n"
    );

    /*
     * The context lives on the bootstrap thread's stack.
     * It remains valid while the bootstrap thread yields
     * and waits for the worker and reaper.
     */
    struct ring3_test_worker_context worker_context = {

        .process = process,

        .address_space = address_space,

        .user_code_page = code_page,

        .user_breakpoint_rip = 0,

        .started = 0,

        .completed = 0,

        .success = 0,

        .bootstrap_resumptions = 0,

        .user_entry = RING3_TEST_CODE_VA
    };

    worker = thread_create_for_process(
        process,
        worker_name,
        ring3_test_user_exception_worker,
        &worker_context,
        0
    );

    if (worker == NULL) {

        console_write(
            "[ FAILED ] Exception-test worker creation\n"
        );

        goto cleanup;
    }

    /*
     * Preserve the ID before enqueueing.
     *
     * The TCB may be destroyed by the reaper before
     * this function resumes.
     */
    uint64_t expected_thread_id = worker->id;

    if (expected_thread_id == 0) {

        console_write(
            "[ FAILED ] Exception-test worker has invalid ID\n"
        );

        goto cleanup;
    }

    if (!scheduler_enqueue_thread(0, worker)) {

        if (worker->state != THREAD_STATE_NEW ||
            worker->run_queue_owner != NULL) {

            console_write(
                "[ FATAL ] Exception-test enqueue left unexpected TCB state\n"
            );

            return false;
        }

        if (!thread_destroy(worker)) {

            console_write(
                "[ FATAL ] Exception-test NEW worker destruction failed\n"
            );

            return false;
        }

        worker = NULL;

        console_write(
            "[ FAILED ] Exception-test worker enqueue\n"
        );

        goto cleanup;
    }

    worker_enqueued = true;

    /*
     * Never dereference this pointer after enqueue.
     */
    worker = NULL;

    /*
     * Wait for both:
     *     - the worker's one-way transition marker;
     *     - process ZOMBIE state with zero attached threads.
     *
     * This includes deferred reaping.
     */
    if (!ring3_test_wait_for_worker(
            process,
            &worker_context,
            cpu
        )) {

        console_write(
            "[ FAILED ] Exception-test completion/reaper timeout\n"
        );

        /*
         * The worker may still be executing.
         * Never unmap its code or stack in this state.
         */
        return false;
    }

    console_write(
        "[ OK ] Exception-test worker terminated and was reaped\n"
    );

    /*
     * Verify that the worker reached the intended
     * one-way user transition.
     */
    if (__atomic_load_n(
            &worker_context.started,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &worker_context.completed,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &worker_context.success,
            __ATOMIC_ACQUIRE
        ) != 1) {

        console_write(
            "[ FAILED ] Exception-test worker setup\n"
        );

        goto cleanup;
    }

    /*
     * Retrieve the durable process-owned exception record.
     *
     * The worker TCB has already been detached and reclaimed.
     */
    process_exception_info_t exception_info = {0};

    if (!process_get_exception_info(
            process,
            &exception_info
        ) ||
        !exception_info.valid) {

        console_write(
            "[ FAILED ] Exception record missing after reaping\n"
        );

        goto cleanup;
    }

    /*
     * Verify exception identity and exact faulting instruction.
     */
    if (exception_info.thread_id != expected_thread_id ||
        exception_info.vector != expected_vector ||
        exception_info.instruction_pointer !=
            expected_faulting_rip ||
        exception_info.fault_address !=
            expected_fault_address) {

        console_write(
            "[ FAILED ] Exception record identity/RIP/address mismatch\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Exception vector, thread ID, RIP and fault address verified\n"
    );

    /*
     * Validate exception-specific error codes.
     *
     * #DE and #UD have no CPU-generated error code.
     * #GP from CLI at CPL3 has error code zero.
     * #PF must report a user-mode, not-present read.
     */
    if (expected_vector == 14) {

        /*
         * Check the five basic x86 page-fault error bits.
         *
         * 0x04: non-present read by user
         * 0x07: write to present read-only user page
         * 0x15: instruction fetch from present NX user page
         * 0x05: read of present supervisor-only page
         */
        if ((exception_info.error_code & 0x1FULL) !=
            expected_page_fault_error) {

            console_write(
                "[ FAILED ] User page-fault error code mismatch\n"
            );

            goto cleanup;
        }

        console_write(
            "[ OK ] User page-fault error code verified\n"
        );

    } else {

        /*
         * #DE and #UD have no CPU-generated error code.
         * The controlled #GP from CLI at CPL3 has error code 0.
         */
        if (exception_info.error_code != 0) {

            console_write(
                "[ FAILED ] User exception error code mismatch\n"
            );

            goto cleanup;
        }

        console_write(
            "[ OK ] User exception error code verified\n"
        );
    }

    /*
     * The exception termination path currently calls:
     *
     *     thread_exit(-1)
     *
     * Verify that exact result survived deferred reaping.
     */
    int64_t observed_exit_status = 0;

    if (!process_get_exit_code(
            process,
            &observed_exit_status
        ) ||
        observed_exit_status !=
            RING3_TEST_EXCEPTION_EXIT_STATUS) {

        console_write(
            "[ FAILED ] Exception process exit status mismatch\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Exception process exit status verified\n"
    );

    /*
     * Verify final process lifecycle state independently.
     */
    process_state_t process_state = PROCESS_STATE_NEW;
    uint32_t thread_count = UINT32_MAX;

    if (!process_get_info(
            process,
            NULL,
            &process_state,
            &thread_count,
            NULL,
            0
        ) ||
        process_state != PROCESS_STATE_ZOMBIE ||
        thread_count != 0) {

        console_write(
            "[ FAILED ] Exception process zombie/thread accounting\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Exception process reached ZOMBIE with zero threads\n"
    );

    /*
     * Verify that the scheduler restored the bootstrap thread
     * and its permanent kernel address space.
     */
    if (scheduler_get_current_thread(0) != bootstrap ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        !ring3_test_kernel_space_active()) {

        console_write(
            "[ FAILED ] Exception-test bootstrap/kernel restoration\n"
        );

        goto cleanup;
    }

    /*
     * Verify both privilege-entry stack configurations.
     */
    uintptr_t observed_rsp0 = 0;
    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_rsp0) ||
        observed_rsp0 != bootstrap_rsp0 ||

        !arch_syscall_get_kernel_stack(
            &observed_syscall_stack
        ) ||

        observed_syscall_stack != bootstrap_rsp0) {

        console_write(
            "[ FAILED ] Exception-test privilege-stack restoration\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Exception-test bootstrap architecture state restored\n"
    );

    success = true;

cleanup:

    /*
     * Never release resources while an enqueued worker
     * might still be executing in the user address space.
     */
    if (worker_enqueued &&
        !ring3_test_kernel_space_active()) {

        console_write(
            "[ FATAL ] Exception-test cleanup refused while user CR3 is active\n"
        );

        return false;
    }

    /*
     * A worker that was created but never enqueued must
     * still be in NEW state before it can be destroyed.
     */
    if (worker != NULL) {

        if (worker->state == THREAD_STATE_NEW &&
            worker->run_queue_owner == NULL) {

            if (!thread_destroy(worker)) {
                success = false;
            }

        } else {

            console_write(
                "[ FATAL ] Exception-test cleanup found a non-NEW worker\n"
            );

            return false;
        }

        worker = NULL;
    }

    /*
     * Release the optional supervisor-only page first.
     *
     * Never free its frame if unmapping fails.
     */
    if (!ring3_test_release_page(
            address_space,
            RING3_TEST_SUPERVISOR_VA,
            supervisor_physical,
            supervisor_mapped
        )) {

        console_write(
            "[ RING3 ] Supervisor test page release failed\n"
        );

        success = false;
    }

    /*
     * Release mappings, physical frames, process registration,
     * address space, and the creator's process reference.
     */
    bool cleanup_success =
        ring3_test_cleanup(
            process,
            address_space,

            code_physical,
            code_mapped,

            stack_physical,
            stack_mapped
        );

    if (!cleanup_success) {
        success = false;
    }

    if (success) {

        console_write("========== ");
        console_write(test_label);
        console_write(" PASSED ==========\n");
    }

    return success;
}

/*
 * Verify that an independent healthy Ring 3 process
 * continues executing after another Ring 3 process faults.
 *
 * Process A:
 *     Enters Ring 3 and executes UD2.
 *     Must be terminated through the user-exception path.
 *
 * Process B:
 *     Enters Ring 3 with interrupts enabled.
 *     Repeatedly yields while waiting for the kernel's
 *     read-only A_REAPED control marker.
 *     Publishes an acknowledgement and exits normally.
 *
 * Coordination:
 *
 *     CONTROL page:
 *         Kernel writable through HHDM.
 *         User readable, read-only.
 *
 *     STATUS page:
 *         User writable.
 *         Kernel observes through HHDM.
 *
 * The test runs on CPU0, consistent with the current
 * process, scheduler, and SYSCALL implementation.
 */
static bool ring3_test_run_fault_isolation(
    struct scheduler_cpu *cpu,
    struct thread *bootstrap,
    uintptr_t bootstrap_rsp0
) {

    console_write(
        "\n========== RING 3 TWO-PROCESS FAULT ISOLATION TEST ==========\n"
    );

    if (cpu == NULL ||
        bootstrap == NULL ||
        scheduler_get_current_thread(0) != bootstrap ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        !ring3_test_kernel_space_active()) {

        console_write(
            "[ FAILED ] Fault-isolation test bootstrap state\n"
        );

        return false;
    }

    /*
     * Record the bootstrap privilege-entry stack configuration.
     */
    uintptr_t initial_rsp0 = 0;

    uintptr_t initial_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&initial_rsp0) ||
        initial_rsp0 != bootstrap_rsp0 ||

        !arch_syscall_get_kernel_stack(
            &initial_syscall_stack
        ) ||

        initial_syscall_stack != bootstrap_rsp0) {

        console_write(
            "[ FAILED ] Fault-isolation initial stack configuration\n"
        );

        return false;
    }

    /*
     * ---------------------------------------------------------
     * Process objects and address spaces
     * ---------------------------------------------------------
     */

    process_t *faulting_process = NULL;

    process_t *survivor_process = NULL;

    vmm_address_space_t *faulting_address_space = NULL;

    vmm_address_space_t *survivor_address_space = NULL;

    /*
     * ---------------------------------------------------------
     * Physical frames
     * ---------------------------------------------------------
     */

    uint64_t faulting_code_physical = 0;

    uint64_t faulting_stack_physical = 0;

    uint64_t survivor_code_physical = 0;

    uint64_t survivor_stack_physical = 0;

    uint64_t control_physical = 0;

    uint64_t status_physical = 0;

    /*
     * ---------------------------------------------------------
     * Mapping ownership flags
     * ---------------------------------------------------------
     */

    bool faulting_code_mapped = false;

    bool faulting_stack_mapped = false;

    bool survivor_code_mapped = false;

    bool survivor_stack_mapped = false;

    bool control_mapped = false;

    bool status_mapped = false;

    /*
     * ---------------------------------------------------------
     * Worker ownership
     * ---------------------------------------------------------
     */

    struct thread *faulting_worker = NULL;

    struct thread *survivor_worker = NULL;

    bool faulting_enqueued = false;

    bool survivor_enqueued = false;

    uint64_t expected_faulting_thread_id = 0;

    uint64_t expected_survivor_thread_id = 0;

    /*
     * ---------------------------------------------------------
     * HHDM aliases
     * ---------------------------------------------------------
     */

    uint8_t *faulting_code_page = NULL;

    uint8_t *survivor_code_page = NULL;

    struct ring3_test_isolation_control *control_page = NULL;

    struct ring3_test_isolation_status *status_page = NULL;

    /*
     * ---------------------------------------------------------
     * Worker contexts
     * ---------------------------------------------------------
     *
     * These objects live on the bootstrap worker's stack.
     * Both remain valid until their processes are reaped.
     */

    struct ring3_test_worker_context faulting_context = {0};

    struct ring3_test_isolation_survivor_context survivor_context = {0};

    /*
     * ---------------------------------------------------------
     * Expected exception information
     * ---------------------------------------------------------
     */

    size_t faulting_program_size = 0;

    uintptr_t expected_faulting_rip = 0;

    uint64_t expected_fault_address = 0;

    /*
     * ---------------------------------------------------------
     * Result state
     * ---------------------------------------------------------
     */

    bool success = false;

    bool faulting_valid = false;

    bool survivor_valid = false;

    bool architecture_valid = false;

    /*
     * ---------------------------------------------------------
     * Create independent user processes.
     * ---------------------------------------------------------
     */

    faulting_process =
        process_create_user("ring3-isolation-faulting");

    if (faulting_process == NULL) {

        console_write(
            "[ FAILED ] Faulting process creation\n"
        );

        goto cleanup;
    }

    survivor_process =
        process_create_user("ring3-isolation-survivor");

    if (survivor_process == NULL) {

        console_write(
            "[ FAILED ] Survivor process creation\n"
        );

        goto cleanup;
    }

    if (!process_get_address_space(
            faulting_process,
            &faulting_address_space
        ) ||
        faulting_address_space == NULL) {

        console_write(
            "[ FAILED ] Faulting process address-space retrieval\n"
        );

        goto cleanup;
    }

    if (!process_get_address_space(
            survivor_process,
            &survivor_address_space
        ) ||
        survivor_address_space == NULL) {

        console_write(
            "[ FAILED ] Survivor process address-space retrieval\n"
        );

        goto cleanup;
    }

    /*
     * ---------------------------------------------------------
     * Allocate six independent physical frames.
     * ---------------------------------------------------------
     */

    faulting_code_physical = pmm_alloc_frame();

    faulting_stack_physical = pmm_alloc_frame();

    survivor_code_physical = pmm_alloc_frame();

    survivor_stack_physical = pmm_alloc_frame();

    control_physical = pmm_alloc_frame();

    status_physical = pmm_alloc_frame();

    if (faulting_code_physical == 0 ||
        faulting_stack_physical == 0 ||
        survivor_code_physical == 0 ||
        survivor_stack_physical == 0 ||
        control_physical == 0 ||
        status_physical == 0) {

        console_write(
            "[ FAILED ] Fault-isolation physical-frame allocation\n"
        );

        goto cleanup;
    }

    /*
     * Initialize every frame before exposing it to user mode.
     */
    if (!ring3_test_zero_frame(faulting_code_physical) ||
        !ring3_test_zero_frame(faulting_stack_physical) ||
        !ring3_test_zero_frame(survivor_code_physical) ||
        !ring3_test_zero_frame(survivor_stack_physical) ||
        !ring3_test_zero_frame(control_physical) ||
        !ring3_test_zero_frame(status_physical)) {

        console_write(
            "[ FAILED ] Fault-isolation frame initialization\n"
        );

        goto cleanup;
    }

    /*
     * Obtain kernel-accessible aliases.
     */
    faulting_code_page =
        (uint8_t *)pmm_frame_to_virt(
            faulting_code_physical
        );

    survivor_code_page =
        (uint8_t *)pmm_frame_to_virt(
            survivor_code_physical
        );

    control_page =
        (struct ring3_test_isolation_control *)
            pmm_frame_to_virt(control_physical);

    status_page =
        (struct ring3_test_isolation_status *)
            pmm_frame_to_virt(status_physical);

    if (faulting_code_page == NULL ||
        survivor_code_page == NULL ||
        control_page == NULL ||
        status_page == NULL) {

        console_write(
            "[ FAILED ] Fault-isolation HHDM mapping\n"
        );

        goto cleanup;
    }

    /*
     * ---------------------------------------------------------
     * Generate Process A's deliberate invalid-opcode program.
     * ---------------------------------------------------------
     */

    if (!ring3_test_build_exception_program(
            faulting_code_page,
            PMM_PAGE_SIZE,
            RING3_EXCEPTION_TEST_INVALID_OPCODE,
            &faulting_program_size,
            &expected_faulting_rip,
            &expected_fault_address
        ) ||
        faulting_program_size == 0 ||
        faulting_program_size > PMM_PAGE_SIZE ||
        expected_faulting_rip == 0) {

        console_write(
            "[ FAILED ] Faulting process program generation\n"
        );

        goto cleanup;
    }

    /*
     * ---------------------------------------------------------
     * Generate Process B's healthy coordination program.
     * ---------------------------------------------------------
     */

    size_t survivor_program_size = 0;

    if (!ring3_test_build_isolation_survivor_program(
            survivor_code_page,
            PMM_PAGE_SIZE,
            &survivor_program_size
        ) ||
        survivor_program_size == 0 ||
        survivor_program_size > PMM_PAGE_SIZE) {

        console_write(
            "[ FAILED ] Survivor process program generation\n"
        );

        goto cleanup;
    }

    /*
     * Make generated instructions visible before execution.
     */
    asm volatile (
        "mfence"
        :
        :
        : "memory"
    );

    /*
     * ---------------------------------------------------------
     * Map Process A's user memory.
     * ---------------------------------------------------------
     */

    if (vmm_address_space_map_page(
            faulting_address_space,
            RING3_TEST_CODE_VA,
            faulting_code_physical,
            VMM_MAP_USER | VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Faulting process code mapping\n"
        );

        goto cleanup;
    }

    faulting_code_mapped = true;

    if (vmm_address_space_map_page(
            faulting_address_space,
            RING3_TEST_STACK_VA,
            faulting_stack_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Faulting process stack mapping\n"
        );

        goto cleanup;
    }

    faulting_stack_mapped = true;

    /*
     * ---------------------------------------------------------
     * Map Process B's independent code and stack.
     * ---------------------------------------------------------
     */

    if (vmm_address_space_map_page(
            survivor_address_space,
            RING3_TEST_CODE_VA,
            survivor_code_physical,
            VMM_MAP_USER | VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Survivor process code mapping\n"
        );

        goto cleanup;
    }

    survivor_code_mapped = true;

    if (vmm_address_space_map_page(
            survivor_address_space,
            RING3_TEST_STACK_VA,
            survivor_stack_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Survivor process stack mapping\n"
        );

        goto cleanup;
    }

    survivor_stack_mapped = true;

    /*
     * ---------------------------------------------------------
     * Map the kernel-to-user control page read-only.
     * ---------------------------------------------------------
     *
     * The kernel updates this page through its HHDM alias.
     * Process B may read it but cannot forge A_REAPED.
     */

    if (vmm_address_space_map_page(
            survivor_address_space,
            RING3_TEST_ISOLATION_CONTROL_VA,
            control_physical,
            VMM_MAP_USER
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Survivor control-page mapping\n"
        );

        goto cleanup;
    }

    control_mapped = true;

    /*
     * ---------------------------------------------------------
     * Map the survivor status page read/write.
     * ---------------------------------------------------------
     *
     * Process B uses this page to publish its actual
     * Ring 3 start and post-fault acknowledgement.
     */

    if (vmm_address_space_map_page(
            survivor_address_space,
            RING3_TEST_ISOLATION_STATUS_VA,
            status_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] Survivor status-page mapping\n"
        );

        goto cleanup;
    }

    status_mapped = true;

    /*
     * ---------------------------------------------------------
     * Verify all effective page permissions.
     * ---------------------------------------------------------
     */

    if (!ring3_test_check_mapping(
            faulting_address_space,
            RING3_TEST_CODE_VA,
            faulting_code_physical,
            false,
            true
        ) ||

        !ring3_test_check_mapping(
            faulting_address_space,
            RING3_TEST_STACK_VA,
            faulting_stack_physical,
            true,
            false
        ) ||

        !ring3_test_check_mapping(
            survivor_address_space,
            RING3_TEST_CODE_VA,
            survivor_code_physical,
            false,
            true
        ) ||

        !ring3_test_check_mapping(
            survivor_address_space,
            RING3_TEST_STACK_VA,
            survivor_stack_physical,
            true,
            false
        ) ||

        !ring3_test_check_mapping(
            survivor_address_space,
            RING3_TEST_ISOLATION_CONTROL_VA,
            control_physical,
            false,
            false
        ) ||

        !ring3_test_check_mapping(
            survivor_address_space,
            RING3_TEST_ISOLATION_STATUS_VA,
            status_physical,
            true,
            false
        )) {

        console_write(
            "[ FAILED ] Fault-isolation user mapping permissions\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Independent process mappings and shared-page permissions verified\n"
    );

    /*
     * ---------------------------------------------------------
     * Prepare the two worker contexts.
     * ---------------------------------------------------------
     */

    faulting_context = (struct ring3_test_worker_context) {

        .process = faulting_process,

        .address_space = faulting_address_space,

        .user_code_page = faulting_code_page,

        .user_breakpoint_rip = 0,

        .started = 0,

        .completed = 0,

        .success = 0,

        .bootstrap_resumptions = 0,

        .user_entry = RING3_TEST_CODE_VA
    };

    survivor_context =
        (struct ring3_test_isolation_survivor_context) {

            .process = survivor_process,

            .address_space = survivor_address_space,

            .started = 0,

            .completed = 0,

            .success = 0
        };

    /*
     * ---------------------------------------------------------
     * Create Process B's worker.
     * ---------------------------------------------------------
     */

    survivor_worker = thread_create_for_process(
        survivor_process,
        "ring3-isolation-survivor-worker",
        ring3_test_isolation_survivor_worker,
        &survivor_context,
        0
    );

    if (survivor_worker == NULL) {

        console_write(
            "[ FAILED ] Survivor worker creation\n"
        );

        goto cleanup;
    }

    expected_survivor_thread_id = survivor_worker->id;

    if (expected_survivor_thread_id == 0) {

        console_write(
            "[ FAILED ] Survivor worker ID\n"
        );

        goto cleanup;
    }

    /*
     * Enqueue Process B first.
     */
    if (!scheduler_enqueue_thread(0, survivor_worker)) {

        if (survivor_worker->state != THREAD_STATE_NEW ||
            survivor_worker->run_queue_owner != NULL) {

            console_write(
                "[ FATAL ] Survivor enqueue left unexpected TCB state\n"
            );

            return false;
        }

        console_write(
            "[ FAILED ] Survivor worker enqueue\n"
        );

        goto cleanup;
    }

    survivor_enqueued = true;

    /*
     * Do not dereference the TCB after enqueue.
     */
    survivor_worker = NULL;

    console_write(
        "[ OK ] Healthy process scheduled before faulting process\n"
    );

    /*
     * ---------------------------------------------------------
     * Wait for Process B's actual Ring 3 start marker.
     * ---------------------------------------------------------
     */

    if (!ring3_test_isolation_wait_for_survivor_start(
            survivor_process,
            status_page,
            cpu
        )) {

        console_write(
            "[ FAILED ] Survivor did not publish its Ring 3 start marker\n"
        );

        goto abort_survivor;
    }

    if (__atomic_load_n(
            &survivor_context.started,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &survivor_context.completed,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &survivor_context.success,
            __ATOMIC_ACQUIRE
        ) != 1) {

        console_write(
            "[ FAILED ] Survivor kernel-side entry setup\n"
        );

        goto abort_survivor;
    }

    console_write(
        "[ OK ] Healthy process confirmed execution in Ring 3\n"
    );

    /*
     * ---------------------------------------------------------
     * Create Process A's faulting worker only after B
     * has confirmed that it is already executing in Ring 3.
     * ---------------------------------------------------------
     */

    faulting_worker = thread_create_for_process(
        faulting_process,
        "ring3-isolation-faulting-worker",
        ring3_test_user_exception_worker,
        &faulting_context,
        0
    );

    if (faulting_worker == NULL) {

        console_write(
            "[ FAILED ] Faulting worker creation\n"
        );

        goto abort_survivor;
    }

    expected_faulting_thread_id = faulting_worker->id;

    if (expected_faulting_thread_id == 0) {

        console_write(
            "[ FAILED ] Faulting worker ID\n"
        );

        goto abort_survivor;
    }

    /*
     * Enqueue Process A.
     */
    if (!scheduler_enqueue_thread(0, faulting_worker)) {

        if (faulting_worker->state != THREAD_STATE_NEW ||
            faulting_worker->run_queue_owner != NULL) {

            console_write(
                "[ FATAL ] Faulting enqueue left unexpected TCB state\n"
            );

            return false;
        }

        console_write(
            "[ FAILED ] Faulting worker enqueue\n"
        );

        goto abort_survivor;
    }

    faulting_enqueued = true;

    /*
     * Never dereference the worker TCB after enqueue.
     */
    faulting_worker = NULL;

    console_write(
        "[ OK ] Faulting process scheduled while survivor remains active\n"
    );

    /*
     * ---------------------------------------------------------
     * Wait for Process A to become ZOMBIE with zero threads.
     * ---------------------------------------------------------
     */

    if (!ring3_test_isolation_wait_for_zombie(
            faulting_process,
            cpu
        )) {

        console_write(
            "[ FAILED ] Faulting process completion/reaper timeout\n"
        );

        goto abort_survivor;
    }

    console_write(
        "[ OK ] Faulting process terminated and was reaped\n"
    );

    /*
     * At this point the kernel has independently observed:
     *
     *     PROCESS_STATE_ZOMBIE
     *     thread_count == 0
     *
     * Publish the control marker only now.
     */
    __atomic_store_n(
        &control_page->faulting_reaped,
        RING3_TEST_ISOLATION_A_REAPED,
        __ATOMIC_RELEASE
    );

    /*
     * ---------------------------------------------------------
     * Verify Process A's exception record and final status.
     * ---------------------------------------------------------
     */

    faulting_valid = true;

    if (__atomic_load_n(
            &faulting_context.started,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &faulting_context.completed,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &faulting_context.success,
            __ATOMIC_ACQUIRE
        ) != 1) {

        faulting_valid = false;
    }

    process_exception_info_t faulting_exception = {0};

    if (!process_get_exception_info(
            faulting_process,
            &faulting_exception
        ) ||
        !faulting_exception.valid ||

        faulting_exception.thread_id !=
            expected_faulting_thread_id ||

        faulting_exception.vector != 6 ||

        faulting_exception.error_code != 0 ||

        faulting_exception.instruction_pointer !=
            expected_faulting_rip ||

        faulting_exception.fault_address !=
            expected_fault_address) {

        faulting_valid = false;
    }

    int64_t faulting_exit_status = 0;

    if (!process_get_exit_code(
            faulting_process,
            &faulting_exit_status
        ) ||
        faulting_exit_status !=
            RING3_TEST_EXCEPTION_EXIT_STATUS) {

        faulting_valid = false;
    }

    if (!ring3_test_isolation_process_reaped(
            faulting_process
        )) {

        faulting_valid = false;
    }

    if (faulting_valid) {

        console_write(
            "[ OK ] Faulting process exception record and exit status verified\n"
        );

    } else {

        console_write(
            "[ FAILED ] Faulting process exception/lifecycle verification\n"
        );
    }

    /*
     * ---------------------------------------------------------
     * Wait for Process B to observe the control marker,
     * acknowledge it, and exit normally.
     * ---------------------------------------------------------
     */

    if (!ring3_test_isolation_wait_for_zombie(
            survivor_process,
            cpu
        )) {

        console_write(
            "[ FAILED ] Survivor did not exit after the faulting process was reaped\n"
        );

        /*
         * Change the control marker to ABORTED.
         *
         * This allows a survivor still waiting in its user loop
         * to leave through its failure exit path.
         */
        __atomic_store_n(
            &control_page->faulting_reaped,
            RING3_TEST_ISOLATION_ABORTED,
            __ATOMIC_RELEASE
        );

        if (!ring3_test_isolation_wait_for_zombie(
                survivor_process,
                cpu
            )) {

            /*
             * B might still be executing. Cleanup will not
             * unmap or free resources used by a live process.
             */
            return false;
        }

        goto cleanup;
    }

    /*
     * ---------------------------------------------------------
     * Verify that Process B actually acknowledged the
     * kernel's post-reaping signal.
     * ---------------------------------------------------------
     */

    survivor_valid = true;

    if (__atomic_load_n(
            &survivor_context.started,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &survivor_context.completed,
            __ATOMIC_ACQUIRE
        ) != 1 ||

        __atomic_load_n(
            &survivor_context.success,
            __ATOMIC_ACQUIRE
        ) != 1) {

        survivor_valid = false;
    }

    if (__atomic_load_n(
            &status_page->survivor_started,
            __ATOMIC_ACQUIRE
        ) != RING3_TEST_ISOLATION_B_STARTED ||

        __atomic_load_n(
            &status_page->survivor_acknowledged,
            __ATOMIC_ACQUIRE
        ) != RING3_TEST_ISOLATION_B_ACKNOWLEDGED ||

        __atomic_load_n(
            &control_page->faulting_reaped,
            __ATOMIC_ACQUIRE
        ) != RING3_TEST_ISOLATION_A_REAPED) {

        survivor_valid = false;
    }

    int64_t survivor_exit_status = 0;

    if (!process_get_exit_code(
            survivor_process,
            &survivor_exit_status
        ) ||
        survivor_exit_status !=
            RING3_TEST_PROCESS_EXIT_EXPECTED_STATUS) {

        survivor_valid = false;
    }

    /*
     * A healthy process must not have acquired an exception record.
     */
    process_exception_info_t survivor_exception = {0};

    if (process_get_exception_info(
            survivor_process,
            &survivor_exception
        )) {

        survivor_valid = false;
    }

    if (!ring3_test_isolation_process_reaped(
            survivor_process
        )) {

        survivor_valid = false;
    }

    if (survivor_valid) {

        console_write(
            "[ OK ] Healthy process resumed after the other process was reaped\n"
        );

        console_write(
            "[ OK ] Healthy process acknowledged termination and exited normally\n"
        );

    } else {

        console_write(
            "[ FAILED ] Healthy process acknowledgement/exit verification\n"
        );
    }

    /*
     * ---------------------------------------------------------
     * Verify bootstrap and kernel architectural-state recovery.
     * ---------------------------------------------------------
     */

    architecture_valid = true;

    if (scheduler_get_current_thread(0) != bootstrap ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING ||
        !ring3_test_kernel_space_active()) {

        architecture_valid = false;
    }

    uintptr_t observed_rsp0 = 0;

    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_rsp0) ||
        observed_rsp0 != bootstrap_rsp0 ||

        !arch_syscall_get_kernel_stack(
            &observed_syscall_stack
        ) ||

        observed_syscall_stack != bootstrap_rsp0) {

        architecture_valid = false;
    }

    /*
     * The SYSCALL assembly handoff slot must be cleared.
     */
    if (arch_syscall_user_rsp_scratch != 0) {
        architecture_valid = false;
    }

    if (architecture_valid) {

        console_write(
            "[ OK ] Bootstrap CR3, TSS.RSP0 and SYSCALL stack restored\n"
        );

    } else {

        console_write(
            "[ FAILED ] Bootstrap architectural-state restoration\n"
        );
    }

    success =
        faulting_valid &&
        survivor_valid &&
        architecture_valid;

    goto cleanup;


/*
 * -------------------------------------------------------------
 * Abort Process B after setup or Process A execution failure.
 * -------------------------------------------------------------
 */
abort_survivor:

    if (survivor_enqueued) {

        /*
         * The read-only control mapping prevents the user process
         * from forging this kernel-originated abort signal.
         */
        __atomic_store_n(
            &control_page->faulting_reaped,
            RING3_TEST_ISOLATION_ABORTED,
            __ATOMIC_RELEASE
        );

        if (!ring3_test_isolation_wait_for_zombie(
                survivor_process,
                cpu
            )) {

            console_write(
                "[ FATAL ] Survivor abort did not complete; resources retained\n"
            );

            return false;
        }
    }

    success = false;


/*
 * -------------------------------------------------------------
 * Resource cleanup.
 * -------------------------------------------------------------
 */
cleanup:

    /*
     * Never unmap resources while either enqueued process
     * might still be executing in its address space.
     */
    if (!ring3_test_kernel_space_active()) {

        console_write(
            "[ FATAL ] Fault-isolation cleanup refused while user CR3 is active\n"
        );

        return false;
    }

    if (faulting_enqueued &&
        !ring3_test_isolation_process_reaped(
            faulting_process
        )) {

        console_write(
            "[ FATAL ] Faulting process is still active; resources retained\n"
        );

        return false;
    }

    if (survivor_enqueued &&
        !ring3_test_isolation_process_reaped(
            survivor_process
        )) {

        console_write(
            "[ FATAL ] Survivor process is still active; resources retained\n"
        );

        return false;
    }

    /*
     * A worker that was created but never enqueued must
     * still be NEW and detached from every run queue.
     */
    if (faulting_worker != NULL) {

        if (faulting_worker->state != THREAD_STATE_NEW ||
            faulting_worker->run_queue_owner != NULL) {

            console_write(
                "[ FATAL ] Faulting worker cleanup found unexpected TCB state\n"
            );

            return false;
        }

        if (!thread_destroy(faulting_worker)) {

            console_write(
                "[ FATAL ] Faulting NEW worker destruction failed\n"
            );

            return false;
        }

        faulting_worker = NULL;
    }

    if (survivor_worker != NULL) {

        if (survivor_worker->state != THREAD_STATE_NEW ||
            survivor_worker->run_queue_owner != NULL) {

            console_write(
                "[ FATAL ] Survivor worker cleanup found unexpected TCB state\n"
            );

            return false;
        }

        if (!thread_destroy(survivor_worker)) {

            console_write(
                "[ FATAL ] Survivor NEW worker destruction failed\n"
            );

            return false;
        }

        survivor_worker = NULL;
    }

    bool cleanup_success = true;

    /*
     * Release the two shared pages before destroying B's
     * address space.
     *
     * Each page is freed only if its unmap succeeds and
     * returns the expected physical frame.
     */
    if (!ring3_test_release_page(
            survivor_address_space,
            RING3_TEST_ISOLATION_CONTROL_VA,
            control_physical,
            control_mapped
        )) {

        console_write(
            "[ RING3 ] Isolation control-page release failed\n"
        );

        cleanup_success = false;
    }

    if (!ring3_test_release_page(
            survivor_address_space,
            RING3_TEST_ISOLATION_STATUS_VA,
            status_physical,
            status_mapped
        )) {

        console_write(
            "[ RING3 ] Isolation status-page release failed\n"
        );

        cleanup_success = false;
    }

    /*
     * Destroy Process B and release its independent code,
     * stack, address space, registry entry and creator ref.
     */
    if (!ring3_test_cleanup(
            survivor_process,
            survivor_address_space,

            survivor_code_physical,
            survivor_code_mapped,

            survivor_stack_physical,
            survivor_stack_mapped
        )) {

        console_write(
            "[ RING3 ] Survivor process cleanup failed\n"
        );

        cleanup_success = false;
    }

    /*
     * Destroy Process A and release its independent code,
     * stack, address space, registry entry and creator ref.
     */
    if (!ring3_test_cleanup(
            faulting_process,
            faulting_address_space,

            faulting_code_physical,
            faulting_code_mapped,

            faulting_stack_physical,
            faulting_stack_mapped
        )) {

        console_write(
            "[ RING3 ] Faulting process cleanup failed\n"
        );

        cleanup_success = false;
    }

    if (success && cleanup_success) {

        console_write(
            "[ OK ] Both independent process address spaces destroyed\n"
        );

        console_write(
            "[ OK ] Both processes' private frames and shared frames released\n"
        );

        console_write(
            "========== TWO-PROCESS FAULT ISOLATION PASSED ==========\n"
        );
    }

    return success && cleanup_success;
}

/* -------------------------------------------------------------------------- */
/* Main Ring 3 integration test                                                */
/* -------------------------------------------------------------------------- */

bool ring3_run_tests(void) {

    console_write(
        "\n========== RING 3 TRANSITION TESTS ==========\n"
    );

    /*
    * The Ring 3 test now executes real SYSCALL instructions.
    */
    if (!arch_syscall_is_enabled()) {

        console_write(
            "[ FAILED ] SYSCALL/SYSRET is not enabled\n"
        );

        return false;
    }

    console_write(
        "[ OK ] SYSCALL/SYSRET configuration verified\n"
    );

    /*
     * NX is essential to verify the non-executable
     * user stack mapping.
     */
    if (!vmm_nx_is_supported() ||
        !vmm_nx_is_enabled()) {

        console_write(
            "[ FAILED ] NX is unavailable or disabled\n"
        );

        return false;
    }

    console_write(
        "[ OK ] NX support and activation verified\n"
    );


    /*
     * Verify the bootstrap/kernel address space is active
     * before constructing another address space.
     */
    if (!ring3_test_kernel_space_active()) {

        console_write(
            "[ FAILED ] Kernel address space is not active\n"
        );

        return false;
    }


    /*
     * Preserve bootstrap state.
     */
    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    struct thread *bootstrap =
        scheduler_get_current_thread(0);

    uintptr_t bootstrap_rsp0 = 0;

    if (cpu == NULL ||
        bootstrap == NULL ||
        !gdt_get_kernel_stack(&bootstrap_rsp0)) {

        console_write(
            "[ FAILED ] Bootstrap scheduler/GDT state invalid\n"
        );

        return false;
    }


    /*
     * Create a real user process with a private address space.
     */
    struct process *process =
        process_create_user("ring3-transition-test");

    if (process == NULL) {

        console_write(
            "[ FAILED ] User process creation\n"
        );

        return false;
    }


    vmm_address_space_t *address_space = NULL;

    uint64_t code_physical = 0;

    uint64_t stack_physical = 0;

    bool code_mapped = false;

    bool stack_mapped = false;

    struct thread *worker = NULL;

    bool worker_enqueued = false;

    bool success = false;


    /*
     * Obtain the process's borrowed address-space pointer.
     *
     * The creator's process reference keeps it valid.
     */
    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        console_write(
            "[ FAILED ] Process address-space retrieval\n"
        );

        goto cleanup;
    }


    /*
     * Allocate separate physical frames for code and stack.
     */
    code_physical = pmm_alloc_frame();

    if (code_physical == 0) {

        console_write(
            "[ FAILED ] User code frame allocation\n"
        );

        goto cleanup;
    }


    stack_physical = pmm_alloc_frame();

    if (stack_physical == 0) {

        console_write(
            "[ FAILED ] User stack frame allocation\n"
        );

        goto cleanup;
    }


    /*
     * Zero both physical frames.
     */
    if (!ring3_test_zero_frame(code_physical) ||
        !ring3_test_zero_frame(stack_physical)) {

        console_write(
            "[ FAILED ] User frame initialization\n"
        );

        goto cleanup;
    }


    /*
     * Install the test program:
     *
     *     INT3
     *
     * The CPU saves the address immediately after this
     * one-byte instruction in the interrupt frame.
     */
    uint8_t *code =
        (uint8_t *)pmm_frame_to_virt(code_physical);

    if (code == NULL) {

        console_write(
            "[ FAILED ] User code HHDM mapping\n"
        );

        goto cleanup;
    }

    /*
    * Generate the full Ring 3 syscall torture program
    * into the code page.
    */
    size_t user_program_size = 0;

    size_t user_breakpoint_offset = 0;

    if (!ring3_test_build_user_program(
            code,
            PMM_PAGE_SIZE,
            &user_program_size,
            &user_breakpoint_offset
        ) ||
        user_program_size == 0 ||
        user_program_size > PMM_PAGE_SIZE - sizeof(hua_process_info_t)  ||
        user_breakpoint_offset >= user_program_size) {

        console_write(
            "[ FAILED ] Ring 3 syscall program generation\n"
        );

        goto cleanup;
    }

    /*
    * The existing breakpoint handler expects the exact
    * address of the final INT3 instruction.
    */
    uintptr_t breakpoint_rip =
        RING3_TEST_CODE_VA + user_breakpoint_offset;

    console_write(
        "[ OK ] Ring 3 syscall torture program generated\n"
    );

    /*
     * Create independent user mappings.
     */
    if (vmm_address_space_map_page(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            VMM_MAP_USER | VMM_MAP_EXECUTABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] User code mapping\n"
        );

        goto cleanup;
    }

    code_mapped = true;


    if (vmm_address_space_map_page(
            address_space,
            RING3_TEST_STACK_VA,
            stack_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        console_write(
            "[ FAILED ] User stack mapping\n"
        );

        goto cleanup;
    }

    stack_mapped = true;


    /*
     * Check actual effective page permissions.
     */
    if (!ring3_test_check_mapping(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            false,
            true
        )) {

        console_write(
            "[ FAILED ] User code permissions or W^X\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] User code is RX and not writable\n"
    );


    if (!ring3_test_check_mapping(
            address_space,
            RING3_TEST_STACK_VA,
            stack_physical,
            true,
            false
        )) {

        console_write(
            "[ FAILED ] User stack permissions or NX\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] User stack is RW and non-executable\n"
    );

    /*
    * Exercise the kernel's user-memory validation and copy
    * functions against this process's actual address space.
    *
    * This runs before the worker is enqueued, so the process
    * creator still owns the process reference and cleanup
    * remains under the Ring 3 test's control.
    */
    if (!user_memory_run_tests(
            address_space,
            RING3_TEST_CODE_VA,
            code_physical,
            RING3_TEST_STACK_VA,
            RING3_TEST_STACK_TOP,
            stack_physical
        )) {

        console_write(
            "[ FAILED ] User-memory access tests\n"
        );

        goto cleanup;
    }

    /*
     * Verify the guard page below the stack is unmapped.
     */
    vmm_mapping_info_t guard_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            RING3_TEST_STACK_VA - PMM_PAGE_SIZE,
            &guard_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        console_write(
            "[ FAILED ] User stack guard page\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] User stack guard page is unmapped\n"
    );


    /*
     * Create process-owned worker.
     *
     * This acquires a process reference for the thread and
     * caches the process's borrowed address-space pointer.
     *
     * The thread is still NEW and is not yet runnable.
     */
    struct ring3_test_worker_context worker_context = {
        .process = process,

        .address_space = address_space,

        .user_code_page =
            (uint8_t *)pmm_frame_to_virt(code_physical),

        .user_breakpoint_rip = breakpoint_rip,

        .started = 0,

        .completed = 0,

        .success = 0,

        .bootstrap_resumptions = 0
    };


    worker = thread_create_for_process(
        process,
        "ring3-user-worker",
        ring3_test_worker,
        &worker_context,
        0
    );

    if (worker == NULL) {

        console_write(
            "[ FAILED ] Process-owned worker creation\n"
        );

        goto cleanup;
    }


    /*
     * Publish the worker to CPU0's runnable queue.
     *
     * The run queue changes NEW -> READY.
     */
    if (!scheduler_enqueue_thread(0, worker)) {

        /*
         * Enqueue failed.
         *
         * A NEW, detached-from-queue thread can still
         * be destroyed safely.
         */
        if (worker->state != THREAD_STATE_NEW ||
            worker->run_queue_owner != NULL) {

            console_write(
                "[ FATAL ] Failed enqueue left unexpected thread state\n"
            );

            /*
             * Do not free an object that might be queued.
             */
            return false;
        }

        (void)process_begin_exit(process);

        if (!thread_destroy(worker)) {

            console_write(
                "[ FATAL ] Cannot detach failed worker\n"
            );

            return false;
        }

        worker = NULL;

        console_write(
            "[ FAILED ] Worker enqueue\n"
        );

        goto cleanup;
    }

    worker_enqueued = true;

    /*
     * Do not inspect the worker pointer after successful
     * enqueueing. The scheduler may execute and reclaim
     * that TCB before this function resumes.
     */
    worker = NULL;


    console_write(
        "[ OK ] Process-owned worker enqueued\n"
    );


    /*
     * Allow the worker to run.
     *
     * The worker executes Ring 3 with interrupts disabled
     * only during the controlled transition.
     *
     * Repeated yielding handles timer preemption that might
     * return to bootstrap before the worker exits.
     */
    if (!ring3_test_wait_for_worker(
            process,
            &worker_context,
            cpu
        )) {

        console_write(
            "[ FAILED ] Worker completion or process reaping timeout\n"
        );

        /*
         * The worker might still be live.
         *
         * Do not unmap its pages, destroy its process,
         * or free any of its frames.
         *
         * main.c will enter its fatal halt path.
         */
        return false;
    }


    console_write(
        "[ OK ] Worker exited and was reaped\n"
    );

    /*
    * Verify the exit status persisted after the worker TCB
    * was detached and reclaimed.
    */
    int64_t observed_exit_status = 0;

    if (!process_get_exit_code(
            process,
            &observed_exit_status
        ) ||
        observed_exit_status !=
            RING3_TEST_THREAD_EXIT_EXPECTED_STATUS) {

        console_write(
            "[ FAILED ] THREAD_EXIT did not preserve the expected exit status\n"
        );

        goto cleanup;

    } else {

        console_write(
            "[ OK ] THREAD_EXIT preserved the signed 64-bit exit status\n"
        );
    }

    /*
     * Verify the worker really executed.
     */
    if (__atomic_load_n(
            &worker_context.started,
            __ATOMIC_ACQUIRE
        ) != 1) {

        console_write(
            "[ FAILED ] Worker never started\n"
        );

        goto cleanup;
    }

    if (__atomic_load_n(
            &worker_context.success,
            __ATOMIC_ACQUIRE
        ) != 1) {

        console_write(
            "[ FAILED ] Ring 3 transition or return\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Genuine Ring 3 breakpoint and Ring 0 return\n"
    );

    /*
    * Verify the actual syscall results written into
    * the user stack by the Ring 3 program.
    */
    if (!ring3_test_verify_syscall_results(
            process,
            stack_physical
        )) {

        console_write(
            "[ FAILED ] Ring 3 syscall tests\n"
        );

        goto cleanup;
    }

    /*
     * Verify scheduler returned to the bootstrap thread.
     */
    if (scheduler_get_current_thread(0) != bootstrap ||
        cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        console_write(
            "[ FAILED ] Bootstrap thread restoration\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Bootstrap thread resumed\n"
    );


    /*
     * Verify the worker's TCB was reaped before attempting
     * to destroy its owning process.
     *
     * The process query must report ZOMBIE and zero threads.
     */
    process_state_t process_state = PROCESS_STATE_NEW;

    uint32_t process_thread_count = UINT32_MAX;

    if (!process_get_info(
            process,
            NULL,
            &process_state,
            &process_thread_count,
            NULL,
            0
        ) ||
        process_state != PROCESS_STATE_ZOMBIE ||
        process_thread_count != 0) {

        console_write(
            "[ FAILED ] Process zombie/reaper accounting\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Process reached ZOMBIE with zero threads\n"
    );


    /*
     * Verify scheduler switched back to the permanent
     * kernel address space.
     */
    if (!ring3_test_kernel_space_active()) {

        console_write(
            "[ FAILED ] Kernel CR3 restoration\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Kernel CR3 restored\n"
    );


    /*
     * Verify the scheduler restored bootstrap TSS.RSP0.
     */
    uintptr_t current_rsp0 = 0;

    if (!gdt_get_kernel_stack(&current_rsp0) ||
        current_rsp0 != bootstrap_rsp0) {

        console_write(
            "[ FAILED ] Bootstrap TSS.RSP0 restoration\n"
        );

        goto cleanup;
    }

    console_write(
        "[ OK ] Bootstrap TSS.RSP0 restored\n"
    );


    success = true;


cleanup:

    /*
     * If enqueue succeeded but the worker has not been
     * confirmed reaped, never perform cleanup.
     *
     * That case already returns above.
     */
    if (worker_enqueued &&
        !ring3_test_kernel_space_active()) {

        console_write(
            "[ FATAL ] Refusing cleanup while user CR3 is active\n"
        );

        return false;
    }


    bool cleanup_success =
        ring3_test_cleanup(
            process,
            address_space,

            code_physical,
            code_mapped,

            stack_physical,
            stack_mapped
        );


    if (!cleanup_success) {
        success = false;
    }

    /*
     * Run an independent Ring 3 PROCESS_EXIT test only after
     * the original THREAD_EXIT process has been cleaned up.
     */
    if (success && cleanup_success) {

        if (!ring3_test_run_process_exit_syscall(
                cpu,
                bootstrap,
                bootstrap_rsp0
            )) {

            success = false;
        }
    }

    /*
     * Verify actual user-mode exception termination.
     *
     * Each test owns an independent process and address space.
     * Every test must complete cleanup before the next begins.
     */

    if (success &&
        !ring3_test_run_user_exception(
            cpu,
            bootstrap,
            bootstrap_rsp0,
            RING3_EXCEPTION_TEST_DIVIDE_ERROR
        )) {

        success = false;
    }

    if (success &&
        !ring3_test_run_user_exception(
            cpu,
            bootstrap,
            bootstrap_rsp0,
            RING3_EXCEPTION_TEST_INVALID_OPCODE
        )) {

        success = false;
    }

    if (success &&
        !ring3_test_run_user_exception(
            cpu,
            bootstrap,
            bootstrap_rsp0,
            RING3_EXCEPTION_TEST_GENERAL_PROTECTION
        )) {

        success = false;
    }

    if (success &&
        !ring3_test_run_user_exception(
            cpu,
            bootstrap,
            bootstrap_rsp0,
            RING3_EXCEPTION_TEST_UNMAPPED_READ
        )) {

        success = false;
    }

    if (success &&
        !ring3_test_run_user_exception(
            cpu,
            bootstrap,
            bootstrap_rsp0,
            RING3_EXCEPTION_TEST_READONLY_WRITE
        )) {

        success = false;
    }

    if (success &&
        !ring3_test_run_user_exception(
            cpu,
            bootstrap,
            bootstrap_rsp0,
            RING3_EXCEPTION_TEST_NX_EXECUTE
        )) {

        success = false;
    }

    if (success &&
        !ring3_test_run_user_exception(
            cpu,
            bootstrap,
            bootstrap_rsp0,
            RING3_EXCEPTION_TEST_SUPERVISOR_READ
        )) {

        success = false;
    }

    /*
    * Verify that a healthy Ring 3 process survives another
    * independent Ring 3 process's exception and reaping.
    */
    if (success &&
        !ring3_test_run_fault_isolation(
            cpu,
            bootstrap,
            bootstrap_rsp0
        )) {

        success = false;
    }

    if (success) {

        console_write(
            "[ OK ] User mappings and physical frames released\n"
        );

        console_write(
            "[ OK ] User process and address space destroyed\n"
        );

        console_write(
            "========== RING 3 TESTS PASSED ==========\n"
        );
    }

    return success;
}
