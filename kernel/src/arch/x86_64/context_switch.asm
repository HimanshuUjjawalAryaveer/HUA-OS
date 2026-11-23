
BITS 64

SECTION .text

GLOBAL arch_context_switch

arch_context_switch:

    ; -------------------------------------------------
    ; Save the current thread's callee-saved registers.
    ;
    ; System V AMD64 arguments:
    ;     RDI = old_context
    ;     RSI = new_context
    ; -------------------------------------------------

    mov [rdi + 0],  r15
    mov [rdi + 8],  r14
    mov [rdi + 16], r13
    mov [rdi + 24], r12

    mov [rdi + 32], rbx
    mov [rdi + 40], rbp

    ; Save the current stack pointer.
    ; It points to this function's return address.
    mov [rdi + 48], rsp


    ; -------------------------------------------------
    ; Restore the next thread's callee-saved registers.
    ; -------------------------------------------------

    mov r15, [rsi + 0]
    mov r14, [rsi + 8]
    mov r13, [rsi + 16]
    mov r12, [rsi + 24]

    mov rbx, [rsi + 32]
    mov rbp, [rsi + 40]

    ; Switch to the next thread's stack.
    mov rsp, [rsi + 48]

    ; Return into the restored thread's execution.
    ret


SECTION .note.GNU-stack noalloc noexec nowrite progbits
