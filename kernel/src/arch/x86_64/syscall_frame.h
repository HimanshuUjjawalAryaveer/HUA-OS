
#ifndef HUA_X86_64_SYSCALL_FRAME_H
#define HUA_X86_64_SYSCALL_FRAME_H

#include <stddef.h>
#include <stdint.h>

/*
 * Register frame constructed by syscall_entry.asm.
 *
 * SYSCALL places:
 *   RCX = user return RIP
 *   R11 = user return RFLAGS
 *
 * The original user RSP is captured in a temporary
 * CPU0 handoff variable and copied into user_rsp.
 *
 * The frame is stored on the calling thread's kernel stack.
 *
 * Keep this layout synchronized with syscall_entry.asm.
 */
struct arch_syscall_frame {

    uint64_t r15;       /*   0 */
    uint64_t r14;       /*   8 */
    uint64_t r13;       /*  16 */
    uint64_t r12;       /*  24 */
    uint64_t r11;       /*  32: user RFLAGS */
    uint64_t r10;       /*  40 */
    uint64_t r9;        /*  48 */
    uint64_t r8;        /*  56 */

    uint64_t rsi;       /*  64 */
    uint64_t rdi;       /*  72 */
    uint64_t rbp;       /*  80 */
    uint64_t rdx;       /*  88 */
    uint64_t rcx;       /*  96: user return RIP */
    uint64_t rbx;       /* 104 */
    uint64_t rax;       /* 112: syscall number / result */

    uint64_t user_rsp;  /* 120 */

};

_Static_assert(
    sizeof(struct arch_syscall_frame) == 128,
    "Syscall frame size does not match assembly"
);

_Static_assert(
    offsetof(struct arch_syscall_frame, rcx) == 96,
    "Syscall RCX offset does not match assembly"
);

_Static_assert(
    offsetof(struct arch_syscall_frame, rax) == 112,
    "Syscall RAX offset does not match assembly"
);

_Static_assert(
    offsetof(struct arch_syscall_frame, user_rsp) == 120,
    "Syscall user RSP offset does not match assembly"
);

#endif
