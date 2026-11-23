BITS 64
DEFAULT REL

SECTION .text

GLOBAL arch_ring3_enter_user
GLOBAL arch_ring3_enter_user_noreturn
GLOBAL arch_ring3_return_trampoline

EXTERN ring3_saved_kernel_rsp


; -----------------------------------------------------------------------------
; Enter Ring 3.
;
; System V AMD64 calling convention:
;
;     RDI = user RIP
;     RSI = user RSP
;
; Return:
;
;     EAX = 1 when the controlled Ring 3 transition
;           returns successfully.
;
;     EAX = 0 when another transition is already active.
;
; This function does not return through user mode.
; The breakpoint interrupt redirects execution to
; arch_ring3_return_trampoline.
; -----------------------------------------------------------------------------

arch_ring3_enter_user:

    ; Only one transition may be active.
    cmp qword [rel ring3_saved_kernel_rsp], 0
    jne .busy


    ; -------------------------------------------------------------------------
    ; Preserve the callee-saved kernel registers.
    ; -------------------------------------------------------------------------

    push rbx
    push rbp

    push r12
    push r13
    push r14
    push r15


    ; -------------------------------------------------------------------------
    ; Record the original kernel stack pointer.
    ;
    ; This is the stack on which the suspended C worker
    ; and its callers are currently executing.
    ; -------------------------------------------------------------------------

    mov [rel ring3_saved_kernel_rsp], rsp


    ; -------------------------------------------------------------------------
    ; Construct the IRETQ privilege-transition frame.
    ;
    ; Push order:
    ;
    ;     SS
    ;     RSP
    ;     RFLAGS
    ;     CS
    ;     RIP
    ;
    ; IRETQ consumes these in the opposite order.
    ; -------------------------------------------------------------------------

    push qword 0x2B       ; GDT_USER_DATA_SELECTOR
    push rsi              ; User RSP
    push qword 0x02       ; Reserved bit 1, IF disabled
    push qword 0x33       ; GDT_USER_CODE_SELECTOR
    push rdi              ; User RIP


    ; -------------------------------------------------------------------------
    ; Do not expose kernel register contents to user mode.
    ;
    ; The original callee-saved values are safely preserved
    ; on the saved kernel stack.
    ;
    ; Caller-saved registers may legally be clobbered by
    ; this function.
    ; -------------------------------------------------------------------------

    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx

    xor rbp, rbp

    xor rdi, rdi
    xor rsi, rsi

    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15


    ; -------------------------------------------------------------------------
    ; Transition:
    ;
    ;     CPL0 -> CPL3
    ;
    ; CPU loads user CS, SS and RSP from the IRETQ frame.
    ; -------------------------------------------------------------------------

    iretq


.busy:

    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; One-way Ring 3 entry.
;
; System V AMD64 calling convention:
;
;     RDI = user RIP
;     RSI = user RSP
;
; This routine deliberately does not:
;
;     - Save a kernel continuation in ring3_saved_kernel_rsp.
;     - Establish a breakpoint-return trampoline.
;     - Preserve callee-saved registers for a kernel return.
;
; It constructs an IRETQ privilege-transition frame and enters
; user mode permanently.
;
; The expected user program terminates its calling thread through
; HUA_SYSCALL_THREAD_EXIT.
;
; If the syscall incorrectly returns, the test program executes
; UD2 so that the existing user-exception termination path catches it.
; -----------------------------------------------------------------------------

arch_ring3_enter_user_noreturn:

    ; Keep maskable interrupts disabled.
    cli

    ; Construct the user-mode IRETQ frame.
    ;
    ; Push order:
    ;
    ;     SS
    ;     RSP
    ;     RFLAGS
    ;     CS
    ;     RIP

    push qword 0x2B
    push rsi
    push qword 0x02
    push qword 0x33
    push rdi

    ; Do not expose kernel register values to user mode.

    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx

    xor rbp, rbp

    xor rdi, rdi
    xor rsi, rsi

    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11

    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    ; Permanently enter CPL3.
    iretq

; -----------------------------------------------------------------------------
; Normal preemptible user-process entry.
;
; System V AMD64 calling convention:
;
;     RDI = user RIP
;     RSI = user RSP
;
; This is a one-way transition, just like the existing
; arch_ring3_enter_user_noreturn().
;
; The important difference is the initial RFLAGS:
;
;     0x00202
;       bit 1  = reserved bit, always set
;       bit 9  = IF, enable maskable interrupts
;
; This allows the LAPIC timer to interrupt user execution and
; lets the scheduler preempt the process normally.
;
; TSS.RSP0 and the SYSCALL kernel stack must already point
; to the current thread's trusted kernel stack.
; -----------------------------------------------------------------------------

global arch_ring3_enter_user_preemptible_noreturn

arch_ring3_enter_user_preemptible_noreturn:

    ; Do not allow an interrupt between preparing the IRETQ
    ; frame and completing the privilege transition.
    cli

    ; Construct the user-mode IRETQ frame.
    ;
    ; Push order:
    ;
    ;     SS
    ;     RSP
    ;     RFLAGS
    ;     CS
    ;     RIP

    push qword 0x2B
    push rsi
    push qword 0x202
    push qword 0x33
    push rdi

    ; Do not expose kernel register contents to user mode.

    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx

    xor rbp, rbp

    xor rdi, rdi
    xor rsi, rsi

    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11

    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    ; Enter CPL3. IRETQ restores IF=1 from the prepared frame.
    iretq

; -----------------------------------------------------------------------------
; Ring 0 return trampoline.
;
; The breakpoint handler rewrites the interrupt frame:
;
;     CS      = GDT_KERNEL_CODE_SELECTOR
;     RIP     = arch_ring3_return_trampoline
;     RFLAGS  = 0x02
;     RSP     = dedicated transition stack top
;     SS      = GDT_KERNEL_DATA_SELECTOR
;
; In 64-bit mode, IRETQ restores all five fields.
;
; Therefore this trampoline begins with RSP pointing
; to the top of the dedicated transition stack.
;
; It then restores the suspended worker's original
; kernel stack and callee-saved registers.
; -----------------------------------------------------------------------------
arch_ring3_return_trampoline:

    ; Remain protected against maskable interrupts.
    cli


    ; Restore the original worker kernel stack.
    mov rsp, [rel ring3_saved_kernel_rsp]


    ; Clear the saved stack pointer.
    mov qword [rel ring3_saved_kernel_rsp], 0


    ; Restore callee-saved registers.
    pop r15
    pop r14
    pop r13
    pop r12

    pop rbp
    pop rbx


    ; The controlled Ring 3 transition succeeded.
    mov eax, 1

    ; Return to the suspended C worker.
    ret
