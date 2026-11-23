#include "idt.h"
#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "interrupt_frame.h"
#include "drivers/console/console.h"
#include "interrupts/irq.h"
#include "scheduler/scheduler.h"
#include "lapic.h"
#include "arch/x86_64/ioapic.h"
#include "ring3.h"
#include "process/process.h"

// -----------------------------------------------------------------------------
// IDT gate descriptor
// -----------------------------------------------------------------------------

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(
    sizeof(struct idt_entry) == 16,
    "An x86_64 IDT entry must be 16 bytes"
);

// -----------------------------------------------------------------------------
// IDTR operand
// -----------------------------------------------------------------------------

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

_Static_assert(
    sizeof(struct idt_pointer) == 10,
    "The x86_64 IDTR operand must be 10 bytes"
);

// -----------------------------------------------------------------------------
// HUA-owned IDT
// -----------------------------------------------------------------------------

static struct idt_entry idt[IDT_ENTRY_COUNT]
    __attribute__((aligned(16)));

// Defined in interrupt_stubs.asm.
extern void (*isr_stub_table[IDT_ENTRY_COUNT])(void);

// Used to verify that the breakpoint handler actually ran.
static volatile uint64_t breakpoint_count = 0;

// Used only by the controlled Invalid Opcode self-test.
static volatile bool invalid_opcode_test_active = false;
static volatile uint64_t invalid_opcode_count = 0;

/*
 * Exact address of the UD2 instruction used by the kernel
 * invalid-opcode self-test.
 *
 * Recovery is permitted only when both CPL0 and this exact
 * instruction address match.
 */
static volatile uintptr_t invalid_opcode_test_rip = 0;


/*
 * Assembly trampoline entered after a user exception has
 * been converted into a kernel thread-exit operation.
 */
extern void arch_user_exception_termination_trampoline(void);

// -----------------------------------------------------------------------------
// Configure one IDT entry
// -----------------------------------------------------------------------------

static void idt_set_gate(
    uint8_t vector,
    uint64_t handler,
    uint8_t ist
) {
    struct idt_entry *entry = &idt[vector];

    entry->offset_low = handler & 0xFFFF;

    entry->selector = GDT_KERNEL_CODE_SELECTOR;

    entry->ist = ist & 0x07;

    /*
    * Present, 64-bit interrupt gates.
    *
    * Vector 3:
    *     0xEE = Present, DPL3, 64-bit interrupt gate.
    *
    * Every other vector:
    *     0x8E = Present, DPL0, 64-bit interrupt gate.
    *
    * Only INT3 is intentionally callable from Ring 3.
    */
    entry->attributes =
        (vector == 3) ? 0xEE : 0x8E;

    entry->offset_middle = (handler >> 16) & 0xFFFF;
    entry->offset_high = (handler >> 32) & 0xFFFFFFFF;

    entry->reserved = 0;
}

// -----------------------------------------------------------------------------
// Install the IDT
// -----------------------------------------------------------------------------

bool idt_init(void) {

    // Keep interrupts disabled during IDT installation.
    asm volatile ("cli" ::: "memory");

    // Every vector receives its own assembly stub.
    for (uint16_t vector = 0;
         vector < IDT_ENTRY_COUNT;
         vector++) {

        uint8_t ist = 0;

        // Double fault: use IST1.
        if (vector == 8) {
            ist = 1;
        }

        // Non-maskable interrupt: use IST2.
        else if (vector == 2) {
            ist = 2;
        }

        // Machine check: use IST3.
        else if (vector == 18) {
            ist = 3;
        }

        // Page fault: use IST4.
        else if (vector == 14) {
            ist = 4;
        }

        uint64_t handler =
            (uint64_t)(uintptr_t)isr_stub_table[vector];

        if (handler == 0) {
            return false;
        }

        idt_set_gate(
            (uint8_t)vector,
            handler,
            ist
        );
    }

    // Construct the IDTR operand.
    struct idt_pointer idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)(uintptr_t)idt
    };

    // Load HUA's IDT.
    asm volatile (
        "lidt %0"
        :
        : "m"(idtr)
        : "memory"
    );

    // Read the active IDTR back.
    struct idt_pointer current_idtr;

    asm volatile (
        "sidt %0"
        : "=m"(current_idtr)
    );

    if (current_idtr.base != idtr.base ||
        current_idtr.limit != idtr.limit) {
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Small hexadecimal diagnostic helper
// -----------------------------------------------------------------------------

static void interrupt_write_hex(uint64_t value) {

    const char digits[] = "0123456789ABCDEF";

    console_write("0x");

    for (int i = 60; i >= 0; i -= 4) {
        uint8_t digit = (value >> i) & 0xF;
        console_put_char(digits[digit]);
    }
}

// -----------------------------------------------------------------------------
// CPU exception names
// -----------------------------------------------------------------------------

static const char *exception_names[32] = {
    "Divide Error",                    // 0
    "Debug",                           // 1
    "Non-Maskable Interrupt",          // 2
    "Breakpoint",                      // 3
    "Overflow",                        // 4
    "BOUND Range Exceeded",            // 5
    "Invalid Opcode",                  // 6
    "Device Not Available",            // 7
    "Double Fault",                    // 8
    "Reserved",                        // 9
    "Invalid TSS",                     // 10
    "Segment Not Present",             // 11
    "Stack-Segment Fault",             // 12
    "General Protection Fault",        // 13
    "Page Fault",                      // 14
    "Reserved",                        // 15
    "x87 Floating-Point Exception",    // 16
    "Alignment Check",                 // 17
    "Machine Check",                   // 18
    "SIMD Floating-Point Exception",   // 19
    "Virtualization Exception",        // 20
    "Control Protection Exception",    // 21
    "Reserved",                        // 22
    "Reserved",                        // 23
    "Reserved",                        // 24
    "Reserved",                        // 25
    "Reserved",                        // 26
    "Reserved",                        // 27
    "Hypervisor Injection Exception",  // 28
    "VMM Communication Exception",     // 29
    "Security Exception",              // 30
    "Reserved"                         // 31
};

// -----------------------------------------------------------------------------
// Page fault error-code diagnostics
// -----------------------------------------------------------------------------

static void interrupt_print_page_fault_error(uint64_t error) {

    console_write("Page fault details:\n");

    console_write("  Protection violation: ");
    console_write((error & (1ULL << 0)) ? "Yes\n" : "No\n");

    console_write("  Write access: ");
    console_write((error & (1ULL << 1)) ? "Yes\n" : "No\n");

    console_write("  User-mode access: ");
    console_write((error & (1ULL << 2)) ? "Yes\n" : "No\n");

    console_write("  Reserved-bit violation: ");
    console_write((error & (1ULL << 3)) ? "Yes\n" : "No\n");

    console_write("  Instruction fetch: ");
    console_write((error & (1ULL << 4)) ? "Yes\n" : "No\n");

    console_write("  Protection key violation: ");
    console_write((error & (1ULL << 5)) ? "Yes\n" : "No\n");

    console_write("  Shadow stack access: ");
    console_write((error & (1ULL << 6)) ? "Yes\n" : "No\n");

    console_write("  SGX violation: ");
    console_write((error & (1ULL << 15)) ? "Yes\n" : "No\n");
}

/*
 * Check for a canonical upper-half kernel address.
 *
 * HUA currently uses four-level x86-64 paging.
 */
static bool interrupt_is_kernel_address(uintptr_t address) {

    uint64_t value = (uint64_t)address;

    return
        (value >> 48) == 0xFFFFULL &&
        (value & (1ULL << 47)) != 0;
}


/*
 * Initial allowlist of user exceptions that HUA knows how
 * to convert into process termination.
 *
 * Other user exceptions remain fatal until explicitly
 * implemented and tested.
 */
static bool interrupt_user_exception_supported(uint64_t vector) {

    switch (vector) {

        case 0:   /* Divide Error */
        case 6:   /* Invalid Opcode */
        case 13:  /* General Protection Fault */
        case 14:  /* Page Fault */
            return true;

        default:
            return false;
    }
}


/*
 * Rewrite a supported user exception's return frame so that
 * IRETQ enters the kernel termination trampoline.
 *
 * This function only prepares the frame. It does not switch
 * contexts or release resources.
 */
static bool interrupt_prepare_user_exception(
    struct interrupt_frame *frame,
    uint64_t fault_address
) {

    if (frame == NULL ||
        !interrupt_user_exception_supported(frame->vector)) {

        return false;
    }

    /*
     * Require the exact user selectors configured by HUA.
     *
     * Merely checking RPL=3 would accept an unexpected
     * selector configuration.
     */
    if (frame->cs != GDT_USER_CODE_SELECTOR ||
        frame->old_ss != GDT_USER_DATA_SELECTOR) {

        return false;
    }

    /*
     * A user-originated page fault must have the U/S bit set
     * in the hardware error code.
     */
    if (frame->vector == 14 &&
        (frame->error_code & (1ULL << 2)) == 0) {

        return false;
    }

    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING ||
        current->process == NULL ||
        current->address_space == NULL ||
        current->preempt_count != 0 ||
        !current->stack_owned ||
        current->stack_base == NULL ||
        current->stack_size < THREAD_STACK_MIN_SIZE) {

        return false;
    }

    /*
     * Verify that the owned stack metadata is internally
     * consistent and that its top is correctly aligned.
     */
    uintptr_t stack_base =
        (uintptr_t)current->stack_base;

    if (stack_base == 0 ||
        stack_base > UINTPTR_MAX - current->stack_size) {

        return false;
    }

    uintptr_t stack_top =
        stack_base + current->stack_size;

    if (current->stack_top != stack_top ||
        (stack_base & 0xFULL) != 0 ||
        (stack_top & 0xFULL) != 0 ||
        !interrupt_is_kernel_address(stack_base) ||
        !interrupt_is_kernel_address(stack_top - 1) ||
        !interrupt_is_kernel_address(stack_top)) {

        return false;
    }

    uintptr_t trampoline =
        (uintptr_t)arch_user_exception_termination_trampoline;

    if (!interrupt_is_kernel_address(trampoline)) {
        return false;
    }

    /*
     * Save the exception in process-owned storage before
     * the TCB can be reaped.
     *
     * process_record_user_exception() also verifies that
     * this is a single-threaded user process and changes
     * its state to EXITING.
     */
    process_exception_info_t information = {

        .valid = false,

        .thread_id = current->id,
        .vector = frame->vector,
        .error_code = frame->error_code,

        .instruction_pointer = frame->rip,
        .fault_address = fault_address
    };

    if (!process_record_user_exception(
            current->process,
            current,
            &information
        )) {

        return false;
    }

    /*
     * Rewrite the normalized frame.
     *
     * The assembly stub will restore all GPRs, including
     * the new RDI value, before executing IRETQ.
     */
    frame->rdi = (uint64_t)(uintptr_t)current;

    frame->rip = (uint64_t)trampoline;
    frame->cs = GDT_KERNEL_CODE_SELECTOR;

    /*
     * Reserved RFLAGS bit 1 must be set.
     *
     * Keep IF, DF, IOPL, TF, NT and other user-controlled
     * flags clear during the termination trampoline.
     */
    frame->rflags = 0x2ULL;

    /*
     * IRETQ will return to the top of the faulting thread's
     * owned kernel stack, leaving the exception or IST stack.
     */
    frame->old_rsp = stack_top;
    frame->old_ss = GDT_KERNEL_DATA_SELECTOR;

    return true;
}

// -----------------------------------------------------------------------------
// Common interrupt dispatcher
// -----------------------------------------------------------------------------

void interrupt_dispatch(struct interrupt_frame *frame) {

    if (frame == NULL) {
        console_write("\n[FATAL] Invalid interrupt frame\n");
        goto halt;
    }

    // -------------------------------------------------------------------------
    // Recoverable breakpoint exception
    // -------------------------------------------------------------------------

    if (frame->vector == 3) {

        breakpoint_count++;

        /*
        * Ring 0 breakpoint:
        *
        * Preserve the existing IDT self-test behavior.
        */
        if ((frame->cs & 3ULL) == 0) {
            return;
        }

        /*
        * Ring 3 breakpoint:
        *
        * Only the controlled Ring 3 test transition
        * is currently supported.
        *
        * Arbitrary user-mode breakpoints must not be
        * allowed to redirect execution into kernel code.
        */
        if ((frame->cs & 3ULL) == 3) {

            if (!ring3_test_handle_breakpoint(frame)) {

                console_write(
                    "\n[FATAL] Unauthorized or invalid Ring 3 breakpoint\n"
                );

                goto halt;
            }

            return;
        }

        /*
        * Unexpected privilege level.
        */
        console_write(
            "\n[FATAL] Invalid breakpoint privilege level\n"
        );

        goto halt;
    }

    // -------------------------------------------------------------------------
    // Controlled Invalid Opcode recovery
    // -------------------------------------------------------------------------

    if (frame->vector == 6 &&
        (frame->cs & 3ULL) == 0 &&
        invalid_opcode_test_active &&
        invalid_opcode_test_rip != 0 &&
        frame->rip == invalid_opcode_test_rip) {

        invalid_opcode_count++;

        /*
        * UD2 is exactly two bytes.
        * Recover only from the exact kernel self-test instruction.
        */
        frame->rip += 2;

        return;
    }

    /*
    * -------------------------------------------------------------------------
    * Supported Ring 3 exception termination
    * -------------------------------------------------------------------------
    *
    * Only selected synchronous exceptions are recoverable.
    * Kernel-originated exceptions never enter this path.
    */
    if (frame->vector < 32 &&
        (frame->cs & 3ULL) == 3 &&
        interrupt_user_exception_supported(frame->vector)) {

        uint64_t fault_address = 0;

        /*
        * CR2 is meaningful for page faults. Capture it before
        * calling any other helper.
        */
        if (frame->vector == 14) {

            asm volatile (
                "mov %%cr2, %0"
                : "=r"(fault_address)
            );
        }

        if (!interrupt_prepare_user_exception(
                frame,
                fault_address
            )) {

            console_write(
                "\n[FATAL] Could not safely terminate user exception\n"
            );

            goto halt;
        }

        /*
        * The common assembly stub will restore registers and
        * execute IRETQ using the rewritten return frame.
        */
        return;
    }

    /*
     * Local APIC spurious interrupt.
     *
     * A spurious interrupt must not receive an EOI.
     */
    if (frame->vector == LAPIC_SPURIOUS_VECTOR) {

        if (lapic_handle_spurious_interrupt()) {
            return;
        }
    }

    // -------------------------------------------------------------------------
    // Generic hardware/software interrupt dispatch
    // -------------------------------------------------------------------------

    if (frame->vector >= IRQ_VECTOR_FIRST &&
        frame->vector <= IRQ_VECTOR_LAST) {

        if (irq_dispatch(frame)) {

            /*
            * Only vectors registered through the I/O APIC
            * routing subsystem receive an I/O APIC EOI.
            *
            * LAPIC timer/error handlers acknowledge themselves.
            * Software interrupts must not receive an EOI.
            */
            ioapic_eoi_for_vector((uint8_t)frame->vector);

            /*
            * The LAPIC timer handler has now completed and sent
            * its EOI. This is the interrupt-return boundary where
            * the scheduler can account the interrupted thread.
            */
            if (frame->vector == LAPIC_TIMER_VECTOR) {
                scheduler_on_timer_interrupt_exit();
            }

            return;
        }
    }

    console_set_color(0xFF5555, 0x101820);

    // -------------------------------------------------------------------------
    // Identify CPU exceptions
    // -------------------------------------------------------------------------

    if (frame->vector < 32) {

        console_write("\n[FATAL] CPU Exception\n");

        console_write("Exception: ");
        console_write(exception_names[frame->vector]);
        console_write("\n");

    } else {

        console_write("\n[FATAL] Unhandled interrupt\n");

        console_write("Interrupt vector: ");
    }

    // -------------------------------------------------------------------------
    // General diagnostics
    // -------------------------------------------------------------------------

    console_write("Vector: ");
    interrupt_write_hex(frame->vector);
    console_write("\n");

    console_write("Error code: ");
    interrupt_write_hex(frame->error_code);
    console_write("\n");

    console_write("RIP: ");
    interrupt_write_hex(frame->rip);
    console_write("\n");

    console_write("CS: ");
    interrupt_write_hex(frame->cs);
    console_write("\n");

    console_write("RFLAGS: ");
    interrupt_write_hex(frame->rflags);
    console_write("\n");

    // -------------------------------------------------------------------------
    // Page Fault diagnostics
    // -------------------------------------------------------------------------

    if (frame->vector == 14) {

        uint64_t cr2;

        asm volatile (
            "mov %%cr2, %0"
            : "=r"(cr2)
        );

        console_write("\nFault address (CR2): ");
        interrupt_write_hex(cr2);
        console_write("\n");

        interrupt_print_page_fault_error(
            frame->error_code
        );
    }

    // -------------------------------------------------------------------------
    // General Protection Fault diagnostics
    // -------------------------------------------------------------------------

    if (frame->vector == 13) {

        uint64_t error = frame->error_code;

        console_write("\nGeneral Protection Fault details:\n");

        console_write("  External event: ");
        console_write((error & 1) ? "Yes\n" : "No\n");

        console_write("  IDT reference: ");
        console_write((error & 2) ? "Yes\n" : "No\n");

        console_write("  Local descriptor table: ");
        console_write((error & 4) ? "Yes\n" : "No\n");

        console_write("  Selector index: ");

        interrupt_write_hex(
            (error >> 3) & 0x1FFF
        );

        console_write("\n");
    }

    console_set_color(0xFFFFFF, 0x101820);

halt:

    // Never return to an unrecoverable exception.
    for (;;) {
        asm volatile ("cli; hlt" ::: "memory");
    }
}

// -----------------------------------------------------------------------------
// IDT self-tests
// -----------------------------------------------------------------------------

bool idt_run_tests(void) {

    // Test 1: Breakpoint exception.
    uint64_t previous_breakpoint_count = breakpoint_count;

    asm volatile ("int3" ::: "memory");

    if (breakpoint_count != previous_breakpoint_count + 1) {
        return false;
    }

    /*
    * Test 2: Controlled Invalid Opcode exception.
    *
    * Publish the exact UD2 instruction address as part of
    * the same assembly block that executes it.
    */
    uint64_t previous_invalid_opcode_count =
        invalid_opcode_count;

    invalid_opcode_test_rip = 0;
    invalid_opcode_test_active = true;

    asm volatile (
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, %0\n\t"
        "1: ud2\n\t"
        : "=m"(invalid_opcode_test_rip)
        :
        : "rax", "memory"
    );

    invalid_opcode_test_active = false;
    invalid_opcode_test_rip = 0;

    if (invalid_opcode_count != previous_invalid_opcode_count + 1) {
        return false;
    }

    return true;
}

/*
 * Finish a user exception after the assembly trampoline
 * has left interrupt/IST context.
 *
 * The function executes on the faulting thread's own
 * kernel stack with interrupts disabled.
 */
__attribute__((noreturn))
void arch_user_exception_terminate(
    struct thread *expected_thread
) {
    asm volatile ("cli" ::: "memory");

    struct thread *current =
        scheduler_get_current_thread(0);

    if (expected_thread == NULL ||
        current == NULL ||
        current != expected_thread ||
        current->state != THREAD_STATE_RUNNING ||
        current->process == NULL ||
        current->address_space == NULL ||
        current->preempt_count != 0 ||
        !current->stack_owned ||
        current->stack_base == NULL) {

        goto fail_stop;
    }

    /*
     * Verify that the active stack belongs to the
     * faulting thread.
     */
    uintptr_t stack_base =
        (uintptr_t)current->stack_base;

    if (stack_base == 0 ||
        current->stack_size < THREAD_STACK_MIN_SIZE ||
        stack_base > UINTPTR_MAX - current->stack_size) {

        goto fail_stop;
    }

    uintptr_t stack_top =
        stack_base + current->stack_size;

    if (current->stack_top != stack_top) {
        goto fail_stop;
    }

    uintptr_t active_rsp = 0;

    asm volatile (
        "mov %%rsp, %0"
        : "=r"(active_rsp)
    );

    if (active_rsp < stack_base ||
        active_rsp >= stack_top) {

        goto fail_stop;
    }

    /*
     * The exception recorder must have moved this
     * single-threaded process into EXITING.
     */
    process_state_t process_state;
    uint32_t process_thread_count = 0;

    if (!process_get_info(
            current->process,
            NULL,
            &process_state,
            &process_thread_count,
            NULL,
            0
        ) ||
        process_state != PROCESS_STATE_EXITING ||
        process_thread_count != 1) {

        goto fail_stop;
    }

    /*
     * Verify that the saved exception belongs to this
     * thread and is one HUA currently supports.
     */
    process_exception_info_t information = {0};

    if (!process_get_exception_info(
            current->process,
            &information
        ) ||
        !information.valid ||
        information.thread_id != current->id ||
        !interrupt_user_exception_supported(
            information.vector
        )) {

        goto fail_stop;
    }

    /*
     * This never returns. The scheduler switches away,
     * and the deferred reaper releases the old thread.
     */
    thread_exit(-1);

fail_stop:

    asm volatile ("cli" ::: "memory");

    for (;;) {
        asm volatile ("hlt" ::: "memory");
    }
}
