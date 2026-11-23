
#ifndef HUA_X86_64_IRQ_FLAGS_H
#define HUA_X86_64_IRQ_FLAGS_H

#include <stdint.h>

/*
 * Save the current RFLAGS and disable maskable interrupts.
 *
 * The returned flags must be passed to
 * arch_irq_restore() after the protected operation.
 *
 * This is an interrupt-state helper, not an SMP lock.
 */
static inline uint64_t arch_irq_save_disable(void) {

    uint64_t flags;

    asm volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}


/*
 * Restore the saved RFLAGS.
 *
 * In particular, this restores the caller's original
 * interrupt-enable state rather than unconditionally
 * enabling interrupts.
 */
static inline void arch_irq_restore(uint64_t flags) {

    asm volatile (
        "pushq %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

#endif
