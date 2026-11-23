
#ifndef HUA_X86_64_CONTEXT_SWITCH_H
#define HUA_X86_64_CONTEXT_SWITCH_H

#include "arch/x86_64/context.h"

/*
 * Switch execution from the current thread to another.
 *
 * old_context:
 *     Receives the current thread's saved CPU context.
 *
 * new_context:
 *     Contains the context to restore.
 *
 * The caller must ensure that scheduler state is protected
 * and interrupts are disabled while performing the switch.
 *
 * This function preserves the System V AMD64 callee-saved
 * registers and the stack pointer.
 */
void arch_context_switch(
    struct arch_thread_context *old_context,
    const struct arch_thread_context *new_context
);

#endif
