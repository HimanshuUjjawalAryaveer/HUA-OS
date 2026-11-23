#ifndef HUA_X86_64_THREAD_BOOTSTRAP_H
#define HUA_X86_64_THREAD_BOOTSTRAP_H

/*
 * Initial execution entry for a newly created thread.
 *
 * This symbol is implemented in NASM.
 */
void arch_thread_bootstrap(void);

/*
 * Emergency trap if a thread bootstrap unexpectedly returns.
 *
 * This symbol is implemented in NASM.
 */
void arch_thread_return_trap(void);

#endif
