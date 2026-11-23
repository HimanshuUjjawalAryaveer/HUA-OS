#ifndef HUA_X86_64_SYSCALL_H
#define HUA_X86_64_SYSCALL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Detect whether the current processor supports
 * the SYSCALL and SYSRET instructions.
 *
 * This function does not enable syscall support
 * or modify any MSR.
 */
bool arch_syscall_cpu_supported(void);

/*
 * Initialize the trusted CPU0 syscall-entry state.
 *
 * Reads the current TSS.RSP0 and publishes it as the
 * trusted kernel stack top used by future syscall entry.
 *
 * Does not enable SYSCALL/SYSRET.
 */
bool arch_syscall_state_init(void);


/*
 * Update the trusted syscall kernel stack.
 *
 * The supplied address must exactly match the currently
 * configured TSS.RSP0 value.
 *
 * Called by the scheduler whenever it selects another thread.
 *
 * Current implementation: CPU0 only.
 */
bool arch_syscall_set_kernel_stack(uintptr_t stack_top);


/*
 * Read the currently published syscall kernel stack.
 */
bool arch_syscall_get_kernel_stack(
    uintptr_t *stack_top_out
);


/*
 * Report whether trusted syscall-entry state was initialized.
 */
bool arch_syscall_state_is_initialized(void);

/*
 * Configure IA32_STAR, IA32_LSTAR and IA32_FMASK,
 * then enable EFER.SCE.
 *
 * Must be called after arch_syscall_state_init()
 * and after the GDT/TSS have been initialized.
 *
 * Current implementation: CPU0 only.
 */
bool arch_syscall_enable(void);

/*
 * Verify that syscall MSRs contain the expected values
 * and EFER.SCE is enabled.
 */
bool arch_syscall_is_enabled(void);

struct thread;
struct arch_syscall_frame;

/*
 * Validate the user state that SYSRETQ will restore.
 *
 * Exported so the syscall security tests can test the
 * production validator directly.
 */
bool arch_syscall_validate_user_return_state(
    struct thread *thread,
    const struct arch_syscall_frame *frame
);

#endif
