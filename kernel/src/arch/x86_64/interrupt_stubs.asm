BITS 64
DEFAULT REL

SECTION .text

EXTERN interrupt_dispatch
EXTERN arch_user_exception_terminate

GLOBAL isr_stub_table

; -----------------------------------------------------------------------------
; Exceptions without a CPU-generated error code
; -----------------------------------------------------------------------------

%macro ISR_NO_ERROR 1

GLOBAL isr_stub_%1

isr_stub_%1:
    ; Add a synthetic error code so every frame has
    ; the same vector/error-code layout.
    push qword 0

    ; Push the interrupt vector.
    push qword %1

    jmp isr_common

%endmacro

; -----------------------------------------------------------------------------
; Exceptions with a CPU-generated error code
; -----------------------------------------------------------------------------

%macro ISR_ERROR_CODE 1

GLOBAL isr_stub_%1

isr_stub_%1:
    ; The CPU already pushed the error code.
    ; Only the vector needs to be added.
    push qword %1

    jmp isr_common

%endmacro

; -----------------------------------------------------------------------------
; Generate all 256 interrupt stubs
; -----------------------------------------------------------------------------

%assign vector 0

%rep 256

    ; Exceptions that push an error code:
    ; 8  - Double Fault
    ; 10 - Invalid TSS
    ; 11 - Segment Not Present
    ; 12 - Stack-Segment Fault
    ; 13 - General Protection Fault
    ; 14 - Page Fault
    ; 17 - Alignment Check
    ; 21 - Control Protection Exception
    ; 29 - VMM Communication Exception (AMD)
    ; 30 - Security Exception
    %if vector = 8 || vector = 10 || vector = 11 || \
        vector = 12 || vector = 13 || vector = 14 || \
        vector = 17 || vector = 21 || vector = 29 || \
        vector = 30

        ISR_ERROR_CODE vector

    %else

        ISR_NO_ERROR vector

    %endif

    %assign vector vector + 1

%endrep

; -----------------------------------------------------------------------------
; Common interrupt entry
; -----------------------------------------------------------------------------

isr_common:

    ; Save all general-purpose registers.
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rdi
    push rsi

    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; RSP now points to the normalized interrupt frame.
    ;
    ; Keep the original frame pointer in RBX.
    ; RBX is callee-saved by the System V AMD64 ABI.
    mov rbx, rsp

    ; First argument: interrupt frame pointer.
    mov rdi, rsp

    ; Align the stack before calling C.
    and rsp, -16

    ; C code expects the direction flag to be clear.
    ; IRETQ will restore the original RFLAGS.
    cld

    call interrupt_dispatch

    ; Restore the original interrupt-frame stack position.
    mov rsp, rbx

    ; Restore general-purpose registers.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8

    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Discard normalized vector and error code.
    add rsp, 16

    ; Restore CPU-saved execution state.
    iretq

; -----------------------------------------------------------------------------
; User exception termination trampoline
; -----------------------------------------------------------------------------
;
; The exception dispatcher rewrites the saved IRETQ frame:
;
;   RIP      = arch_user_exception_termination_trampoline
;   CS       = kernel code selector
;   RFLAGS   = 0x2
;   old_rsp  = faulting thread's kernel stack top
;   old_ss   = kernel data selector
;   RDI      = faulting thread's TCB
;
; IRETQ therefore leaves the exception/IST stack and enters
; this trampoline on the faulting thread's own kernel stack.
;
; The stack top is 16-byte aligned. That is the required
; alignment immediately before CALL under the System V ABI.
;
; This is now ordinary Ring 0 thread execution, not interrupt
; handler execution. It is safe to call thread_exit().

GLOBAL arch_user_exception_termination_trampoline

arch_user_exception_termination_trampoline:

    cld

    ; RDI contains the trusted TCB pointer placed by the
    ; kernel exception dispatcher.
    call arch_user_exception_terminate

    ; The C function is noreturn. Reaching here is corruption.
    cli

.hua_user_exception_termination_halt:

    hlt
    jmp .hua_user_exception_termination_halt

; -----------------------------------------------------------------------------
; Table of all 256 stub addresses
; -----------------------------------------------------------------------------

SECTION .rodata align=8

%macro ISR_TABLE_ENTRY 1
    dq isr_stub_%1
%endmacro

isr_stub_table:

%assign vector 0

%rep 256

    ISR_TABLE_ENTRY vector

    %assign vector vector + 1

%endrep
