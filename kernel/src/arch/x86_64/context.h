
#ifndef HUA_X86_64_CONTEXT_H
#define HUA_X86_64_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Cooperative x86-64 thread context.
 *
 * The context-switch assembly routine preserves the
 * System V AMD64 callee-saved general-purpose registers.
 *
 * RSP points to the return address of the suspended
 * context-switch call.
 *
 * The return address itself remains on the thread stack.
 */
struct arch_thread_context {

    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;

    uint64_t rbx;
    uint64_t rbp;

    uint64_t rsp;
};

/*
 * Assembly-visible structure offsets.
 *
 * If this structure changes, these assertions catch
 * layout changes that would break the assembly routine.
 */
_Static_assert(
    offsetof(struct arch_thread_context, r15) == 0,
    "Invalid context r15 offset"
);

_Static_assert(
    offsetof(struct arch_thread_context, r14) == 8,
    "Invalid context r14 offset"
);

_Static_assert(
    offsetof(struct arch_thread_context, r13) == 16,
    "Invalid context r13 offset"
);

_Static_assert(
    offsetof(struct arch_thread_context, r12) == 24,
    "Invalid context r12 offset"
);

_Static_assert(
    offsetof(struct arch_thread_context, rbx) == 32,
    "Invalid context rbx offset"
);

_Static_assert(
    offsetof(struct arch_thread_context, rbp) == 40,
    "Invalid context rbp offset"
);

_Static_assert(
    offsetof(struct arch_thread_context, rsp) == 48,
    "Invalid context rsp offset"
);

_Static_assert(
    sizeof(struct arch_thread_context) == 56,
    "Invalid context structure size"
);

#endif
