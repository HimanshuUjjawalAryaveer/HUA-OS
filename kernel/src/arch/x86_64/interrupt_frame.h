
#ifndef HUA_INTERRUPT_FRAME_H
#define HUA_INTERRUPT_FRAME_H

#include <stdint.h>

/*
 * Normalized x86-64 interrupt frame.
 *
 * Assembly pushes 15 general-purpose registers, followed
 * by the interrupt vector and error code.
 *
 * The CPU saves:
 *
 *     RIP
 *     CS
 *     RFLAGS
 *     old RSP
 *     old SS
 *
 * IMPORTANT:
 *
 * In 64-bit mode, the CPU pushes old SS:RSP
 * unconditionally, including interrupts that originate
 * at the same privilege level.
 *
 * In memory order, these fields therefore follow RFLAGS:
 *
 *     old_rsp
 *     old_ss
 */

struct interrupt_frame {

    /* General-purpose registers. */

    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;

    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    /* Normalized interrupt information. */

    uint64_t vector;
    uint64_t error_code;

    /* CPU-saved return state. */

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;

    /* Original stack context saved by the CPU. */

    uint64_t old_rsp;
    uint64_t old_ss;
};

/*
 * 15 GPRs + 2 normalized fields + 5 CPU-saved fields.
 *
 * Keep this synchronized with interrupt_stubs.asm.
 */
_Static_assert(
    sizeof(struct interrupt_frame) == 22ULL * sizeof(uint64_t),
    "Interrupt frame layout does not match x86-64 assembly"
);

#endif
