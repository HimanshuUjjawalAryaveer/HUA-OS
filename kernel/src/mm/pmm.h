#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

#define PMM_PAGE_SIZE 4096ULL
#define PMM_PAGE_SHIFT 12

// Initialize the PMM using Limine's memory information.
bool pmm_init(
    struct limine_memmap_response *memory_map,
    uint64_t hhdm_offset
);

// Allocate one 4 KiB physical frame.
// Returns its physical address, or 0 on failure.
uint64_t pmm_alloc_frame(void);

// Release a frame previously allocated by the PMM.
bool pmm_free_frame(uint64_t physical_address);

// Convert an allocated physical frame into its HHDM virtual address.
void *pmm_frame_to_virt(uint64_t physical_address);

// Return the number of currently free frames.
uint64_t pmm_get_free_frames(void);

/*
 * Test-only one-shot failure injection for releasing one
 * exact allocated physical frame.
 */
bool pmm_test_fail_free_frame_once(
    uint64_t physical_address
);

void pmm_test_clear_free_frame_failure(void);

/*
 * Test-only one-shot failure injection after N successful
 * frame releases. Zero fails the next valid release.
 *
 * This replaces any previously armed exact-frame failure.
 */
bool pmm_test_fail_free_after(
    uint64_t successful_frees
);

uint64_t pmm_test_get_last_failed_free_frame(void);

/*
 * Test-only one-shot physical-frame allocation failure.
 */
bool pmm_test_fail_alloc_after(
    uint64_t successful_allocations
);

void pmm_test_clear_alloc_failure(void);

void pmm_test_clear_free_after_failure(void);

#endif
