
BITS 64
DEFAULT REL

SECTION .text

GLOBAL arch_syscall_entry

EXTERN arch_syscall_kernel_stack_top
EXTERN arch_syscall_user_rsp_scratch
EXTERN arch_syscall_dispatch


; -----------------------------------------------------------------------------
; x86-64 SYSCALL entry
;
; On entry:
;   RCX = user return RIP
;   R11 = user return RFLAGS
;   RSP = user RSP
;
; SYSCALL does not switch stacks or push an interrupt frame.
;
; The entry path therefore:
;   1. Saves user RSP in a temporary CPU0 handoff slot.
;   2. Switches to the current thread's trusted kernel stack.
;   3. Constructs a 128-byte register frame.
;   4. Clears the temporary handoff slot.
;   5. Calls the C dispatcher.
;   6. Restores user state and executes SYSRETQ.
;
; The dispatcher may yield. The frame remains on the calling
; thread's own kernel stack while that thread is suspended.
; -----------------------------------------------------------------------------

arch_syscall_entry:

    ; Save user RSP before changing stacks.
    ;
    ; The kernel data is in the shared supervisor-only
    ; upper-half mapping, including in process address spaces.
    mov [rel arch_syscall_user_rsp_scratch], rsp

    ; Switch to the trusted stack selected by the scheduler.
    mov rsp, [rel arch_syscall_kernel_stack_top]

    ; Allocate the complete frame.
    ;
    ; The stack top is 16-byte aligned.
    ; Subtracting 128 preserves that alignment before CALL.
    sub rsp, 128

    ; Save general-purpose registers.
    mov [rsp + 0],   r15
    mov [rsp + 8],   r14
    mov [rsp + 16],  r13
    mov [rsp + 24],  r12
    mov [rsp + 32],  r11
    mov [rsp + 40],  r10
    mov [rsp + 48],  r9
    mov [rsp + 56],  r8

    mov [rsp + 64],  rsi
    mov [rsp + 72],  rdi
    mov [rsp + 80],  rbp
    mov [rsp + 88],  rdx
    mov [rsp + 96],  rcx
    mov [rsp + 104], rbx
    mov [rsp + 112], rax

    ; Move user RSP into the thread-owned frame.
    mov rax, [rel arch_syscall_user_rsp_scratch]
    mov [rsp + 120], rax

    ; The shared handoff slot must not retain syscall state
    ; while C executes or while a thread is suspended.
    mov qword [rel arch_syscall_user_rsp_scratch], 0

    ; C argument 1 = pointer to the saved register frame.
    mov rdi, rsp

    ; The kernel C ABI requires DF=0.
    ; The original user flags are preserved in frame.r11.
    cld

    ; RSP is 16-byte aligned before CALL.
    call arch_syscall_dispatch

    ; C return value becomes the user-visible RAX result.
    mov [rsp + 112], rax

    ; Restore every register except the syscall-clobbered
    ; RCX and R11, which are restored as return RIP/RFLAGS.
    mov r15, [rsp + 0]
    mov r14, [rsp + 8]
    mov r13, [rsp + 16]
    mov r12, [rsp + 24]

    mov r10, [rsp + 40]
    mov r9,  [rsp + 48]
    mov r8,  [rsp + 56]

    mov rsi, [rsp + 64]
    mov rdi, [rsp + 72]
    mov rbp, [rsp + 80]
    mov rdx, [rsp + 88]
    mov rbx, [rsp + 104]

    ; SYSRETQ consumes RCX and R11 as the user RIP and RFLAGS.
    mov rcx, [rsp + 96]
    mov r11, [rsp + 32]

    ; Restore the syscall result.
    mov rax, [rsp + 112]

    ; Restore user RSP last.
    ;
    ; The memory operand is evaluated using the old kernel
    ; RSP before the destination RSP is updated.
    mov rsp, [rsp + 120]

    ; Return to CPL3.
    o64 sysret
