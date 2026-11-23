BITS 64
DEFAULT REL

SECTION .text

GLOBAL gdt_load

gdt_load:
    ; RDI = address of the 10-byte GDTR structure.
    ; RSI = TSS selector.

    ; Load HUA's Global Descriptor Table.
    lgdt [rdi]

    ; Reload CS using a far return.
    ; A normal near return cannot reload CS.
    push qword 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:

    ; Reload the kernel data segment registers.
    mov ax, 0x10

    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load the Task Register with HUA's TSS selector.
    mov ax, si
    ltr ax

    ret
