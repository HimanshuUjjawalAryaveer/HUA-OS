#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "arch/x86_64/syscall.h"
#include "arch/x86_64/gdt.h"


/*
 * CPUID extended processor information.
 */
#define ARCH_CPUID_MAX_EXTENDED_LEAF  0x80000000U
#define ARCH_CPUID_EXTENDED_FEATURES  0x80000001U

/*
 * CPUID 0x80000001 EDX:
 *
 * Bit 11 = SYSCALL/SYSRET support.
 */
#define ARCH_CPUID_SYSCALL_BIT        (1U << 11)

extern void arch_syscall_entry(void);

/*
 * --------------------------------------------------------------------------
 * Trusted CPU0 syscall-entry state
 * --------------------------------------------------------------------------
 *
 * The assembly syscall entry routine will read
 * arch_syscall_kernel_stack_top before switching RSP.
 *
 * Keep this symbol globally visible because NASM
 * references it directly.
 *
 * The initial stack is HUA's dedicated bootstrap
 * transition stack. The scheduler subsequently updates
 * it to the selected thread's kernel stack.
 */

volatile uintptr_t arch_syscall_kernel_stack_top
    __attribute__((aligned(8))) = 0;


/*
 * Temporary CPU0-only handoff for user RSP.
 *
 * Assembly uses this only between the first instruction of
 * syscall entry and the point at which user RSP is copied
 * into the thread-owned syscall frame.
 *
 * It must be cleared before calling C. It must never retain
 * state across a dispatcher call or context switch.
 */
volatile uintptr_t arch_syscall_user_rsp_scratch
    __attribute__((aligned(8))) = 0;


/*
 * Initialization state.
 *
 * Accessed atomically because this state is shared
 * between C and the architecture entry code.
 */
static uint32_t arch_syscall_state_ready = 0;


/*
 * Validate an upper-half kernel stack address.
 *
 * HUA currently uses four-level x86-64 paging.
 */
static bool arch_syscall_validate_kernel_stack(
    uintptr_t stack_top
) {

    if (stack_top == 0 ||
        (stack_top & 0xFULL) != 0) {

        return false;
    }

    uint64_t address = (uint64_t)stack_top;

    /*
     * Canonical upper-half address:
     *
     * Bits 63:48 must extend bit 47.
     */
    if ((address >> 47) != 0x1FFFFULL) {
        return false;
    }

    return true;
}

/*
 * Validate that the assembly entry point resides in HUA's
 * canonical upper-half kernel address range.
 */
static bool arch_syscall_validate_kernel_entry(
    uintptr_t entry
) {

    if (entry == 0) {
        return false;
    }

    return ((uint64_t)entry >> 47) == 0x1FFFFULL;
}

/*
 * Initialize the trusted syscall-entry state.
 *
 * This function must execute after gdt_init().
 */
bool arch_syscall_state_init(void) {

    __atomic_store_n(
        &arch_syscall_state_ready,
        0,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &arch_syscall_kernel_stack_top,
        0,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &arch_syscall_user_rsp_scratch,
        0,
        __ATOMIC_RELEASE
    );

    /*
     * SYSCALL/SYSRET must be supported before
     * initializing the corresponding entry state.
     */
    if (!arch_syscall_cpu_supported()) {
        return false;
    }

    /*
    * Verify the NASM entry symbol is linked into the kernel.
    *
    * Taking its address also keeps the entry section reachable
    * when the linker performs section garbage collection.
    */
    if (!arch_syscall_validate_kernel_entry(
            (uintptr_t)arch_syscall_entry
        )) {

        return false;
    }

    /*
     * Obtain the currently configured TSS.RSP0.
     *
     * Immediately after GDT initialization this points
     * to the dedicated bootstrap transition stack.
     */
    uintptr_t stack_top = 0;

    if (!gdt_get_kernel_stack(&stack_top)) {
        return false;
    }

    if (!arch_syscall_validate_kernel_stack(stack_top)) {
        return false;
    }

    /*
     * Publish the initial trusted stack.
     */
    __atomic_store_n(
        &arch_syscall_kernel_stack_top,
        stack_top,
        __ATOMIC_RELEASE
    );

    /*
     * Publish initialization last.
     */
    __atomic_store_n(
        &arch_syscall_state_ready,
        1,
        __ATOMIC_RELEASE
    );

    return true;
}


/*
 * Update the trusted syscall kernel stack.
 *
 * The scheduler calls this after updating TSS.RSP0.
 *
 * We intentionally require an exact match with TSS.RSP0.
 * This prevents the syscall entry stack and the hardware
 * privilege-transition stack from becoming inconsistent.
 */
bool arch_syscall_set_kernel_stack(
    uintptr_t stack_top
) {

    if (__atomic_load_n(
            &arch_syscall_state_ready,
            __ATOMIC_ACQUIRE
        ) != 1) {

        return false;
    }

    if (!arch_syscall_validate_kernel_stack(stack_top)) {
        return false;
    }

    /*
     * Verify the active hardware TSS configuration.
     */
    uintptr_t tss_stack_top = 0;

    if (!gdt_get_kernel_stack(&tss_stack_top)) {
        return false;
    }

    if (tss_stack_top != stack_top) {
        return false;
    }

    /*
     * Publish the new kernel stack.
     */
    __atomic_store_n(
        &arch_syscall_kernel_stack_top,
        stack_top,
        __ATOMIC_RELEASE
    );

    return true;
}


/*
 * Retrieve the trusted syscall kernel stack.
 */
bool arch_syscall_get_kernel_stack(
    uintptr_t *stack_top_out
) {

    if (stack_top_out == NULL) {
        return false;
    }

    if (__atomic_load_n(
            &arch_syscall_state_ready,
            __ATOMIC_ACQUIRE
        ) != 1) {

        return false;
    }

    uintptr_t stack_top =
        __atomic_load_n(
            &arch_syscall_kernel_stack_top,
            __ATOMIC_ACQUIRE
        );

    if (!arch_syscall_validate_kernel_stack(stack_top)) {
        return false;
    }

    *stack_top_out = stack_top;

    return true;
}


/*
 * Report initialization state.
 */
bool arch_syscall_state_is_initialized(void) {

    return __atomic_load_n(
        &arch_syscall_state_ready,
        __ATOMIC_ACQUIRE
    ) == 1;
}

/*
 * Execute CPUID.
 *
 * Inputs:
 *     leaf
 *     subleaf
 *
 * Outputs:
 *     EAX, EBX, ECX, EDX
 *
 * CPUID is available on x86-64 processors.
 */
static void arch_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax_out,
    uint32_t *ebx_out,
    uint32_t *ecx_out,
    uint32_t *edx_out
) {

    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;

    asm volatile (
        "cpuid"
        : "=a"(eax),
          "=b"(ebx),
          "=c"(ecx),
          "=d"(edx)
        : "a"(leaf),
          "c"(subleaf)
        : "memory"
    );

    if (eax_out != NULL) {
        *eax_out = eax;
    }

    if (ebx_out != NULL) {
        *ebx_out = ebx;
    }

    if (ecx_out != NULL) {
        *ecx_out = ecx;
    }

    if (edx_out != NULL) {
        *edx_out = edx;
    }
}


/*
 * Detect SYSCALL/SYSRET support.
 *
 * CPUID 0x80000001 EDX bit 11 advertises support.
 *
 * We first check the maximum supported extended CPUID
 * leaf so that we never query an unavailable leaf.
 */
bool arch_syscall_cpu_supported(void) {

    uint32_t maximum_extended_leaf = 0;

    arch_cpuid(
        ARCH_CPUID_MAX_EXTENDED_LEAF,
        0,
        &maximum_extended_leaf,
        NULL,
        NULL,
        NULL
    );

    if (maximum_extended_leaf <
        ARCH_CPUID_EXTENDED_FEATURES) {

        return false;
    }

    uint32_t extended_features_edx = 0;

    arch_cpuid(
        ARCH_CPUID_EXTENDED_FEATURES,
        0,
        NULL,
        NULL,
        NULL,
        &extended_features_edx
    );

    return
        (extended_features_edx &
         ARCH_CPUID_SYSCALL_BIT) != 0;
}


/* -------------------------------------------------------------------------- */
/* SYSCALL/SYSRET MSR configuration                                           */
/* -------------------------------------------------------------------------- */

#define ARCH_SYSCALL_MSR_EFER   0xC0000080U
#define ARCH_SYSCALL_MSR_STAR   0xC0000081U
#define ARCH_SYSCALL_MSR_LSTAR  0xC0000082U
#define ARCH_SYSCALL_MSR_FMASK  0xC0000084U

#define ARCH_SYSCALL_EFER_SCE   (1ULL << 0)

/*
 * SYSCALL clears these bits from RFLAGS on entry.
 *
 * IF:  prevent maskable interrupts before the entry stack
 *      and register frame are established.
 *
 * DF:  guarantee the kernel C ABI's direction-flag requirement.
 *
 * TF:  avoid a debug trap in the entry sequence.
 *
 * IOPL, NT, VM, AC, VIF, VIP and RF are also cleared.
 *
 * The user's original RFLAGS are still preserved in R11
 * by the processor and copied into the syscall frame.
 */
#define ARCH_SYSCALL_FMASK_VALUE ( \
    (1ULL << 8)  |  /* TF   */ \
    (1ULL << 9)  |  /* IF   */ \
    (1ULL << 10) |  /* DF   */ \
    (3ULL << 12) |  /* IOPL */ \
    (1ULL << 14) |  /* NT   */ \
    (1ULL << 16) |  /* RF   */ \
    (1ULL << 17) |  /* VM   */ \
    (1ULL << 18) |  /* AC   */ \
    (1ULL << 19) |  /* VIF  */ \
    (1ULL << 20)    /* VIP  */ \
)

/*
 * Read a model-specific register.
 */
static uint64_t arch_syscall_read_msr(uint32_t msr) {

    uint32_t low = 0;
    uint32_t high = 0;

    asm volatile (
        "rdmsr"
        : "=a"(low),
          "=d"(high)
        : "c"(msr)
        : "memory"
    );

    return ((uint64_t)high << 32) | low;
}

/*
 * Write a model-specific register.
 */
static void arch_syscall_write_msr(
    uint32_t msr,
    uint64_t value
) {

    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    asm volatile (
        "wrmsr"
        :
        : "c"(msr),
          "a"(low),
          "d"(high)
        : "memory"
    );
}

/*
 * Construct STAR from HUA's actual GDT selectors.
 *
 * SYSCALL:
 *     CS = STAR[47:32] & ~3
 *     SS = CS + 8
 *
 * SYSRET:
 *     CS = STAR[63:48] + 16
 *     SS = STAR[63:48] + 8
 *
 * HUA's selectors:
 *     Kernel CS = 0x08
 *     User SS   = 0x2B
 *     User CS   = 0x33
 *
 * Therefore the SYSRET base is 0x23.
 */
static bool arch_syscall_build_star(
    uint64_t *star_out
) {

    if (star_out == NULL) {
        return false;
    }

    uint16_t kernel_cs =
        (uint16_t)GDT_KERNEL_CODE_SELECTOR;

    uint16_t user_ss =
        (uint16_t)GDT_USER_DATA_SELECTOR;

    uint16_t user_cs =
        (uint16_t)GDT_USER_CODE_SELECTOR;

    if ((kernel_cs & 7U) != 0U ||
        (user_ss & 3U) != 3U ||
        (user_cs & 3U) != 3U) {

        return false;
    }

    if (user_cs < 16U) {
        return false;
    }

    uint16_t sysret_base =
        (uint16_t)(user_cs - 16U);

    if ((uint16_t)(sysret_base + 8U) != user_ss ||
        (uint16_t)(sysret_base + 16U) != user_cs) {

        return false;
    }

    *star_out =
        ((uint64_t)sysret_base << 48) |
        ((uint64_t)kernel_cs << 32);

    return true;
}

/*
 * Verify the complete syscall configuration.
 */
bool arch_syscall_is_enabled(void) {

    if (!arch_syscall_cpu_supported() ||
        !arch_syscall_state_is_initialized()) {

        return false;
    }

    uint64_t expected_star = 0;

    if (!arch_syscall_build_star(&expected_star)) {
        return false;
    }

    uint64_t expected_lstar =
        (uint64_t)(uintptr_t)arch_syscall_entry;

    uint64_t efer =
        arch_syscall_read_msr(ARCH_SYSCALL_MSR_EFER);

    if ((efer & ARCH_SYSCALL_EFER_SCE) == 0) {
        return false;
    }

    return
        arch_syscall_read_msr(ARCH_SYSCALL_MSR_STAR)
            == expected_star &&

        arch_syscall_read_msr(ARCH_SYSCALL_MSR_LSTAR)
            == expected_lstar &&

        arch_syscall_read_msr(ARCH_SYSCALL_MSR_FMASK)
            == ARCH_SYSCALL_FMASK_VALUE;
}

/*
 * Configure and enable SYSCALL/SYSRET.
 */
bool arch_syscall_enable(void) {

    if (!arch_syscall_cpu_supported() ||
        !arch_syscall_state_is_initialized()) {

        return false;
    }

    /*
     * Validate the entry point.
     */
    uintptr_t entry =
        (uintptr_t)arch_syscall_entry;

    if (!arch_syscall_validate_kernel_entry(entry)) {
        return false;
    }

    /*
     * The trusted syscall stack must match TSS.RSP0.
     */
    uintptr_t syscall_stack = 0;
    uintptr_t tss_stack = 0;

    if (!arch_syscall_get_kernel_stack(&syscall_stack) ||
        !gdt_get_kernel_stack(&tss_stack) ||
        syscall_stack != tss_stack ||
        !arch_syscall_validate_kernel_stack(syscall_stack)) {

        return false;
    }

    /*
     * Construct and validate STAR.
     */
    uint64_t star = 0;

    if (!arch_syscall_build_star(&star)) {
        return false;
    }

    uint64_t lstar = (uint64_t)entry;

    uint64_t fmask = ARCH_SYSCALL_FMASK_VALUE;

    /*
     * Disable SCE while configuring the other MSRs.
     *
     * Preserve all other EFER bits, including NXE and LME/LMA.
     */
    uint64_t old_efer =
        arch_syscall_read_msr(ARCH_SYSCALL_MSR_EFER);

    uint64_t disabled_efer =
        old_efer & ~ARCH_SYSCALL_EFER_SCE;

    arch_syscall_write_msr(
        ARCH_SYSCALL_MSR_EFER,
        disabled_efer
    );

    if ((arch_syscall_read_msr(ARCH_SYSCALL_MSR_EFER) &
         ARCH_SYSCALL_EFER_SCE) != 0) {

        return false;
    }

    /*
     * Configure the syscall entry and return selectors.
     */
    arch_syscall_write_msr(
        ARCH_SYSCALL_MSR_STAR,
        star
    );

    /*
     * LSTAR is the 64-bit kernel entry point.
     */
    arch_syscall_write_msr(
        ARCH_SYSCALL_MSR_LSTAR,
        lstar
    );

    /*
     * FMASK defines the RFLAGS bits cleared on entry.
     */
    arch_syscall_write_msr(
        ARCH_SYSCALL_MSR_FMASK,
        fmask
    );

    /*
     * Verify all three MSRs before enabling SCE.
     */
    if (arch_syscall_read_msr(ARCH_SYSCALL_MSR_STAR) != star ||
        arch_syscall_read_msr(ARCH_SYSCALL_MSR_LSTAR) != lstar ||
        arch_syscall_read_msr(ARCH_SYSCALL_MSR_FMASK) != fmask) {

        return false;
    }

    /*
     * Enable the SYSCALL instruction as the final step.
     */
    arch_syscall_write_msr(
        ARCH_SYSCALL_MSR_EFER,
        disabled_efer | ARCH_SYSCALL_EFER_SCE
    );

    if (!arch_syscall_is_enabled()) {

        /*
         * Do not leave syscall entry enabled with
         * an unverified configuration.
         */
        arch_syscall_write_msr(
            ARCH_SYSCALL_MSR_EFER,
            disabled_efer
        );

        return false;
    }

    return true;
}
