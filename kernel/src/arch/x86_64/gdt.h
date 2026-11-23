#ifndef HUA_GDT_H
#define HUA_GDT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Kernel privilege-level selectors.
 */
#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_TSS_SELECTOR         0x18

/*
 * User privilege-level selectors.
 *
 * RPL = 3.
 */
#define GDT_USER_DATA_SELECTOR   0x2B
#define GDT_USER_CODE_SELECTOR   0x33

/*
 * GDT descriptor layout.
 */
#define GDT_USER_DATA_INDEX      5U
#define GDT_USER_CODE_INDEX      6U
#define GDT_ENTRY_COUNT          7U

/*
 * Install HUA's GDT and load its Task State Segment.
 *
 * Initially configures RSP0 to use the bootstrap
 * privilege-transition stack.
 */
bool gdt_init(void);

/*
 * Update TSS.RSP0 to the supplied kernel stack top.
 *
 * The address must be nonzero, 16-byte aligned, and
 * canonical in the upper half of the current 4-level
 * x86-64 virtual address space.
 *
 * Current implementation: CPU0 only.
 */
bool gdt_set_kernel_stack(uintptr_t stack_top);

/*
 * Restore the original bootstrap privilege-transition stack.
 *
 * Used when the bootstrap thread becomes the selected thread.
 */
bool gdt_restore_bootstrap_stack(void);

/*
 * Read the currently configured TSS.RSP0.
 */
bool gdt_get_kernel_stack(uintptr_t *stack_top_out);

/*
 * TEST HOOK:
 * Read a GDT descriptor for architecture self-tests.
 */
bool gdt_test_get_descriptor(
    uint32_t index,
    uint64_t *descriptor_out
);

#endif
