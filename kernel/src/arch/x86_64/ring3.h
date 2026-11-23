#ifndef HUA_RING3_H
#define HUA_RING3_H

#include <stdbool.h>
#include <stdint.h>

struct thread;
struct interrupt_frame;

/*
 * Enter user mode at the supplied instruction pointer
 * with the supplied user stack pointer.
 *
 * Returns true only when the controlled Ring 3 breakpoint
 * handler successfully redirects execution back to the
 * kernel return trampoline.
 *
 * Current implementation:
 *     - CPU0 only.
 *     - One active transition at a time.
 *     - Used by the initial Ring 3 self-test.
 */
bool arch_ring3_enter_user(
    uint64_t user_rip,
    uint64_t user_rsp
);

/*
 * Enter Ring 3 without establishing a kernel return path.
 *
 * RDI = user RIP
 * RSI = user RSP
 *
 * This routine never returns.
 *
 * It is intended for controlled one-way transitions,
 * such as testing a non-returning thread-exit syscall.
 *
 * It does not use ring3_saved_kernel_rsp or the
 * controlled breakpoint-return trampoline.
 */
_Noreturn void arch_ring3_enter_user_noreturn(
    uint64_t user_rip,
    uint64_t user_rsp
);

/*
 * Enter Ring 3 permanently with maskable interrupts enabled.
 *
 * RDI = user RIP
 * RSI = user RSP
 *
 * Unlike the controlled test transition, this is intended for
 * ordinary scheduler-managed user processes.
 *
 * The scheduler must already have activated the process address
 * space and configured TSS.RSP0 and the SYSCALL kernel stack.
 */
_Noreturn void arch_ring3_enter_user_preemptible_noreturn(
    uint64_t user_rip,
    uint64_t user_rsp
);

/*
 * Arm the controlled user breakpoint test.
 *
 * The currently executing thread must be the supplied
 * process-owned worker.
 *
 * transition_stack_top must be the currently configured
 * TSS.RSP0 value.
 *
 * Interrupts must already be disabled.
 */
bool ring3_test_arm(
    struct thread *thread,
    uintptr_t transition_stack_top,
    uintptr_t user_rip,
    uintptr_t user_rsp
);


/*
 * Handle the controlled Ring 3 breakpoint.
 *
 * Returns true only when the interrupt frame is valid
 * and has been redirected to the Ring 0 return trampoline.
 */
bool ring3_test_handle_breakpoint(
    struct interrupt_frame *frame
);


/*
 * Verify that the controlled breakpoint was successfully
 * handled and that the assembly trampoline restored the
 * saved kernel stack.
 */
bool ring3_test_was_hit(void);


/*
 * Clear the test's active context.
 */
void ring3_test_disarm(void);

#endif
