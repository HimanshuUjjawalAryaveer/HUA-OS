#ifndef HUA_X86_64_MSR_H
#define HUA_X86_64_MSR_H

#include <stdint.h>

/*
 * --------------------------------------------------------------------------
 * Common x86-64 Model-Specific Registers
 * --------------------------------------------------------------------------
 *
 * MSRs are privileged CPU registers accessed with RDMSR and WRMSR.
 *
 * RDMSR:
 *     ECX = MSR number
 *     EDX:EAX = 64-bit result
 *
 * WRMSR:
 *     ECX = MSR number
 *     EDX:EAX = 64-bit value
 *
 * These instructions must execute at CPL0.
 */

#define ARCH_MSR_IA32_EFER       0xC0000080U

#define ARCH_MSR_IA32_STAR       0xC0000081U
#define ARCH_MSR_IA32_LSTAR      0xC0000082U
#define ARCH_MSR_IA32_FMASK      0xC0000084U

#define ARCH_MSR_IA32_FS_BASE    0xC0000100U
#define ARCH_MSR_IA32_GS_BASE    0xC0000101U
#define ARCH_MSR_IA32_KERNEL_GS_BASE 0xC0000102U


/*
 * IA32_EFER bit definitions.
 */

#define ARCH_EFER_SCE            (1ULL << 0)
#define ARCH_EFER_LME            (1ULL << 8)
#define ARCH_EFER_LMA            (1ULL << 10)
#define ARCH_EFER_NXE            (1ULL << 11)


/*
 * Read a 64-bit Model-Specific Register.
 *
 * IMPORTANT:
 *
 * The caller must supply a valid MSR supported by the processor.
 * Reading an unsupported or reserved MSR can generate #GP.
 *
 * This function does not provide fault recovery.
 */
static inline uint64_t arch_msr_read(uint32_t msr) {

    uint32_t low = 0;
    uint32_t high = 0;

    asm volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
        : "memory"
    );

    return ((uint64_t)high << 32) | low;
}


/*
 * Write a 64-bit Model-Specific Register.
 *
 * IMPORTANT:
 *
 * The caller is responsible for:
 * - Selecting a valid MSR.
 * - Checking processor support.
 * - Supplying architecturally valid values.
 * - Preserving reserved bits where required.
 *
 * This function does not validate the MSR or value.
 */
static inline void arch_msr_write(
    uint32_t msr,
    uint64_t value
) {

    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    asm volatile (
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
        : "memory"
    );
}

#endif
