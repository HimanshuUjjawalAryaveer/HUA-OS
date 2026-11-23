#include "gdt.h"

#include <stdint.h>
#include <stddef.h>
#include "arch/x86_64/irq_flags.h"

#define GDT_STACK_SIZE  (16 * 1024)
#define GDT_STACK_ALIGN 4096

// -----------------------------------------------------------------------------
// 64-bit x86 Task State Segment
// -----------------------------------------------------------------------------

struct tss64 {
    uint32_t reserved0;

    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;

    uint64_t reserved1;

    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;

    uint64_t reserved2;

    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(
    sizeof(struct tss64) == 104,
    "The x86_64 TSS must be exactly 104 bytes"
);

// -----------------------------------------------------------------------------
// GDT pointer loaded by LGDT
// -----------------------------------------------------------------------------

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

_Static_assert(
    sizeof(struct gdt_pointer) == 10,
    "The x86_64 GDTR operand must be 10 bytes"
);

// -----------------------------------------------------------------------------
// HUA-owned descriptor tables and stacks
// -----------------------------------------------------------------------------

/*
 * Seven GDT entries:
 *
 *   0: Null descriptor
 *   1: Kernel code
 *   2: Kernel data
 *   3: TSS descriptor (lower 8 bytes)
 *   4: TSS descriptor (upper 8 bytes)
 *   5: User data
 *   6: User code
 */
static uint64_t gdt[GDT_ENTRY_COUNT]
    __attribute__((aligned(16)));

/*
 * True only after GDT loading and hardware verification
 * have completed successfully.
 */
static bool gdt_initialized = false;

static struct tss64 tss __attribute__((aligned(16)));

// Separate stacks for privilege transitions and critical exceptions.
static uint8_t ring0_stack[GDT_STACK_SIZE]
    __attribute__((aligned(GDT_STACK_ALIGN)));

static uint8_t double_fault_stack[GDT_STACK_SIZE]
    __attribute__((aligned(GDT_STACK_ALIGN)));

static uint8_t nmi_stack[GDT_STACK_SIZE]
    __attribute__((aligned(GDT_STACK_ALIGN)));

static uint8_t machine_check_stack[GDT_STACK_SIZE]
    __attribute__((aligned(GDT_STACK_ALIGN)));

static uint8_t page_fault_stack[GDT_STACK_SIZE]
    __attribute__((aligned(GDT_STACK_ALIGN)));

// Implemented in gdt_load.asm.
extern void gdt_load(
    const struct gdt_pointer *gdtr,
    uint16_t tss_selector
);

// -----------------------------------------------------------------------------
// Build a 64-bit available TSS descriptor
// -----------------------------------------------------------------------------

static void gdt_set_tss_descriptor(
    uint64_t *low,
    uint64_t *high,
    uint64_t base,
    uint32_t limit
) {
    *low =
        ((uint64_t)(limit & 0xFFFF)) |
        ((base & 0xFFFFFF) << 16) |
        (0x89ULL << 40) |
        ((uint64_t)((limit >> 16) & 0x0F) << 48) |
        (((base >> 24) & 0xFF) << 56);

    *high = base >> 32;
}

// -----------------------------------------------------------------------------
// Initialize HUA's GDT and TSS
// -----------------------------------------------------------------------------

bool gdt_init(void) {

    // Do not permit interrupts during descriptor-table replacement.
    asm volatile ("cli" ::: "memory");

    /*
    * Clear every descriptor and the TSS before configuring
    * the new descriptor table.
    */
    for (size_t i = 0; i < GDT_ENTRY_COUNT; i++) {
        gdt[i] = 0;
    }

    gdt_initialized = false;

    tss = (struct tss64){0};

    // -------------------------------------------------------------------------
    // GDT entries
    // -------------------------------------------------------------------------

    // Entry 0: Null descriptor.
    gdt[0] = 0;

    /*
    * Kernel code descriptor.
    * Accessed bit is pre-set.
    */
    gdt[1] = 0x00AF9B000000FFFFULL;

    /*
    * Kernel data descriptor.
    * Accessed bit is pre-set.
    */
    gdt[2] = 0x00CF93000000FFFFULL;

    /*
    * User data descriptor.
    * Accessed bit is pre-set.
    */
    gdt[GDT_USER_DATA_INDEX] =
        0x00CFF3000000FFFFULL;

    /*
    * User code descriptor.
    * Accessed bit is pre-set.
    */
    gdt[GDT_USER_CODE_INDEX] =
        0x00AFFB000000FFFFULL;

    // -------------------------------------------------------------------------
    // Configure the TSS stacks
    // -------------------------------------------------------------------------

    tss.rsp0 =
        (uint64_t)(uintptr_t)(ring0_stack + GDT_STACK_SIZE);

    tss.ist1 =
        (uint64_t)(uintptr_t)(double_fault_stack + GDT_STACK_SIZE);

    tss.ist2 =
        (uint64_t)(uintptr_t)(nmi_stack + GDT_STACK_SIZE);

    tss.ist3 =
        (uint64_t)(uintptr_t)(machine_check_stack + GDT_STACK_SIZE);

    tss.ist4 =
        (uint64_t)(uintptr_t)(page_fault_stack + GDT_STACK_SIZE);

    // No I/O permission bitmap is provided.
    // Its base is set to the end of the TSS.
    tss.iomap_base = sizeof(struct tss64);

    // -------------------------------------------------------------------------
    // Install the 16-byte TSS descriptor
    // -------------------------------------------------------------------------

    uint64_t tss_base = (uint64_t)(uintptr_t)&tss;

    gdt_set_tss_descriptor(
        &gdt[3],
        &gdt[4],
        tss_base,
        sizeof(struct tss64) - 1
    );

    // -------------------------------------------------------------------------
    // Construct the GDTR operand
    // -------------------------------------------------------------------------

    struct gdt_pointer gdtr = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)(uintptr_t)gdt
    };

    // -------------------------------------------------------------------------
    // Load GDT, reload segment registers, and load TR
    // -------------------------------------------------------------------------

    gdt_load(&gdtr, GDT_TSS_SELECTOR);

    // -------------------------------------------------------------------------
    // Verify CPU register state
    // -------------------------------------------------------------------------

    struct gdt_pointer current_gdtr;
    uint16_t current_tr;

    asm volatile ("sgdt %0" : "=m"(current_gdtr));
    asm volatile ("str %0" : "=r"(current_tr));

    if (current_gdtr.base != gdtr.base ||
        current_gdtr.limit != gdtr.limit) {
        return false;
    }

    if (current_tr != GDT_TSS_SELECTOR) {
        return false;
    }

    // Verify the configured stack pointers.
    if (tss.rsp0 == 0 ||
        tss.ist1 == 0 ||
        tss.ist2 == 0 ||
        tss.ist3 == 0 ||
        tss.ist4 == 0) {
        return false;
    }

    if ((tss.rsp0 & 0xF) != 0 ||
        (tss.ist1 & 0xF) != 0 ||
        (tss.ist2 & 0xF) != 0 ||
        (tss.ist3 & 0xF) != 0 ||
        (tss.ist4 & 0xF) != 0) {
        return false;
    }

    gdt_initialized = true;

    return true;
}

/*
 * Validate a kernel stack top.
 *
 * HUA currently uses four-level paging:
 * bits 63:48 must be the sign extension of bit 47.
 *
 * Kernel stack addresses must reside in the canonical
 * upper half and be 16-byte aligned.
 */
static bool gdt_validate_kernel_stack(
    uintptr_t stack_top
) {

    if (stack_top == 0 ||
        (stack_top & 0xFULL) != 0) {

        return false;
    }

    uint64_t address = (uint64_t)stack_top;

    /*
     * For a canonical upper-half 48-bit address:
     *
     * address >> 47 == 0x1FFFF
     */
    if ((address >> 47) != 0x1FFFFULL) {
        return false;
    }

    return true;
}


/*
 * Configure the kernel stack used by the next
 * privilege-level transition to Ring 0.
 *
 * The scheduler must call this before transferring
 * execution to the selected thread.
 */
bool gdt_set_kernel_stack(uintptr_t stack_top) {

    if (!gdt_initialized ||
        !gdt_validate_kernel_stack(stack_top)) {

        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    /*
     * The current implementation has one active CPU
     * and one TSS.
     *
     * Interrupts are disabled so this CPU cannot begin
     * another privilege transition while RSP0 is updated.
     */
    tss.rsp0 = (uint64_t)stack_top;

    arch_irq_restore(flags);

    return true;
}


/*
 * Restore the bootstrap privilege-transition stack.
 *
 * The Limine bootstrap thread uses a borrowed stack whose
 * bounds are not known to HUA. Until the bootstrap thread
 * enters user mode (which it will never do), retain the
 * original dedicated transition stack.
 */
bool gdt_restore_bootstrap_stack(void) {

    if (!gdt_initialized) {
        return false;
    }

    uintptr_t stack_top =
        (uintptr_t)(ring0_stack + GDT_STACK_SIZE);

    if (!gdt_validate_kernel_stack(stack_top)) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    tss.rsp0 = (uint64_t)stack_top;

    arch_irq_restore(flags);

    return true;
}


/*
 * Return the active TSS.RSP0 value.
 */
bool gdt_get_kernel_stack(
    uintptr_t *stack_top_out
) {

    if (!gdt_initialized ||
        stack_top_out == NULL) {

        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    *stack_top_out = (uintptr_t)tss.rsp0;

    arch_irq_restore(flags);

    return true;
}


/*
 * TEST HOOK:
 * Read an entry from HUA's active GDT.
 */
bool gdt_test_get_descriptor(
    uint32_t index,
    uint64_t *descriptor_out
) {

    if (!gdt_initialized ||
        index >= GDT_ENTRY_COUNT ||
        descriptor_out == NULL) {

        return false;
    }

    *descriptor_out = gdt[index];

    return true;
}
