BITS 64

SECTION .text

GLOBAL arch_thread_bootstrap
GLOBAL arch_thread_return_trap

EXTERN thread_bootstrap


; ---------------------------------------------------------------------------
; Initial entry for a new kernel thread.
;
; Context-switch assembly restores:
;     R12 = pointer to struct thread
;     RSP = address of arch_thread_bootstrap
;
; RET enters this function with:
;     RSP % 16 == 8
;
; Before CALL, we subtract 8 so that RSP % 16 == 0.
; The CALL then gives the C function the required entry alignment.
; ---------------------------------------------------------------------------

arch_thread_bootstrap:

    ; Pass the TCB pointer as the first C argument.
    mov rdi, r12

    ; Align RSP for a normal System V AMD64 CALL.
    sub rsp, 8

    ; This C function must never return.
    call thread_bootstrap

    ; Returning is a programming error.
    ud2


; ---------------------------------------------------------------------------
; Emergency return trap.
;
; A thread bootstrap must never return through this address.
; ---------------------------------------------------------------------------

arch_thread_return_trap:

.return_halt:

    cli
    hlt
    jmp .return_halt


SECTION .note.GNU-stack noalloc noexec nowrite progbits
