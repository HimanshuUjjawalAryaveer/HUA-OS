#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "mm/pmm.h"
#include "lib/memory.h"
#include "arch/x86_64/irq_flags.h"

#define PMM_BITS_PER_BYTE 8ULL
#define PMM_RESERVED_LOW_MEMORY 0x100000ULL

static uint8_t *frame_bitmap = 0;
static uint8_t *allocation_bitmap = 0;

static uint64_t total_frames = 0;
static uint64_t free_frames = 0;

// Next frame index where the allocator should begin searching.
static uint64_t next_search_frame = 0;

static uint64_t bitmap_size = 0;
static uint64_t bitmap_storage_size = 0;

static uint64_t hhdm_offset = 0;
static uint64_t bitmap_physical_address = 0;

static bool initialized = false;

/*
 * Test-only one-shot physical-frame release failure.
 *
 * When the selected allocated frame is freed, the operation
 * fails once without changing either PMM bitmap or free_frames.
 *
 * Zero means disabled because PMM never allocates physical frame 0.
 */
static uint64_t pmm_test_free_failure_physical_address = 0;

/*
 * Test-only one-shot countdown failure for frame release.
 *
 * When enabled, the PMM allows the configured number of
 * successful frame releases, then fails the next valid release.
 *
 * The PMM lock protects both fields.
 */
static bool pmm_test_free_failure_countdown_enabled = false;
static uint64_t pmm_test_free_successes_remaining = 0;

static uint64_t pmm_test_last_failed_free_physical_address = 0;

/*
 * Test-only physical-frame allocation failure injection.
 *
 * After N successful allocations, the next allocation
 * fails exactly once.
 */
static bool pmm_test_alloc_failure_countdown_enabled = false;

static uint64_t pmm_test_alloc_successes_remaining = 0;

/*
 * PMM synchronization.
 *
 * This lock deliberately does not use scheduler-aware
 * spinlocks because PMM is initialized before the scheduler.
 *
 * Local interrupts are disabled while waiting for or holding
 * the lock. Atomic operations provide cross-CPU exclusion.
 */
static uint32_t pmm_lock = 0;


static bool pmm_lock_acquire(uint64_t *flags_out) {

    if (flags_out == NULL) {
        return false;
    }

    uint64_t flags = arch_irq_save_disable();

    for (;;) {

        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(
                &pmm_lock,
                &expected,
                1,
                false,
                __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED)) {

            *flags_out = flags;
            return true;
        }

        while (__atomic_load_n(
                   &pmm_lock,
                   __ATOMIC_RELAXED) != 0) {

            asm volatile ("pause" ::: "memory");
        }
    }
}


static void pmm_lock_release(uint64_t flags) {

    __atomic_store_n(
        &pmm_lock,
        0,
        __ATOMIC_RELEASE
    );

    arch_irq_restore(flags);
}

// Set a frame's occupied/reserved bit.
static void bitmap_set(uint8_t *bitmap, uint64_t frame) {
    bitmap[frame / PMM_BITS_PER_BYTE] |=
        (uint8_t)(1U << (frame % PMM_BITS_PER_BYTE));
}


// Clear a frame's bit.
static void bitmap_clear(uint8_t *bitmap, uint64_t frame) {
    bitmap[frame / PMM_BITS_PER_BYTE] &=
        (uint8_t)~(1U << (frame % PMM_BITS_PER_BYTE));
}


// Check whether a frame's bit is set.
static bool bitmap_test(const uint8_t *bitmap, uint64_t frame) {
    return (bitmap[frame / PMM_BITS_PER_BYTE] &
            (uint8_t)(1U << (frame % PMM_BITS_PER_BYTE))) != 0;
}

/*
 * Arm one failure for release of an exact allocated frame.
 *
 * The PMM lock protects both the allocation check and
 * installation of the one-shot failure.
 */
bool pmm_test_fail_free_frame_once(
    uint64_t physical_address
) {
    if (physical_address == 0 ||
        (physical_address & (PMM_PAGE_SIZE - 1ULL)) != 0) {

        return false;
    }

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return false;
    }

    uint64_t frame =
        physical_address >> PMM_PAGE_SHIFT;

    bool valid =
        initialized &&
        frame < total_frames &&
        allocation_bitmap != NULL &&
        bitmap_test(allocation_bitmap, frame);

    if (valid) {
        pmm_test_free_failure_physical_address =
            physical_address;

        /*
        * The two failure-injection modes are mutually exclusive.
        */
        pmm_test_free_failure_countdown_enabled = false;
        pmm_test_free_successes_remaining = 0;
        pmm_test_last_failed_free_physical_address = 0;
    }

    pmm_lock_release(irq_flags);

    return valid;
}

/*
 * Disable PMM frame-release failure injection.
 */
void pmm_test_clear_free_frame_failure(void) {
    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return;
    }

    pmm_test_free_failure_physical_address = 0;

    pmm_lock_release(irq_flags);
}

/*
 * Arm a one-shot failure after a specified number of
 * successful frame releases.
 *
 * successful_frees == 0 means the next valid free fails.
 *
 * This replaces any previously armed exact-frame failure.
 */
bool pmm_test_fail_free_after(uint64_t successful_frees) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return false;
    }

    if (!initialized) {
        pmm_lock_release(irq_flags);
        return false;
    }

    pmm_test_free_failure_physical_address = 0;

    pmm_test_free_successes_remaining =
        successful_frees;

    pmm_test_free_failure_countdown_enabled = true;

    pmm_test_last_failed_free_physical_address = 0;

    pmm_lock_release(irq_flags);

    return true;
}


/*
 * Disable the countdown frame-release failure.
 */
void pmm_test_clear_free_after_failure(void) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return;
    }

    pmm_test_free_failure_countdown_enabled = false;
    pmm_test_free_successes_remaining = 0;

    pmm_lock_release(irq_flags);
}

// Align a number upwards.
static bool align_up(
    uint64_t value,
    uint64_t alignment,
    uint64_t *result
) {
    if (value > UINT64_MAX - (alignment - 1)) {
        return false;
    }

    *result = (value + alignment - 1) & ~(alignment - 1);

    return true;
}


// Find a usable physical region for both bitmaps.
static bool find_bitmap_region(
    struct limine_memmap_response *memory_map
) {
    for (uint64_t i = 0; i < memory_map->entry_count; i++) {

        struct limine_memmap_entry *entry =
            memory_map->entries[i];

        if (entry == 0 ||
            entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        if (entry->length > UINT64_MAX - entry->base) {
            continue;
        }

        uint64_t region_end = entry->base + entry->length;

        uint64_t minimum_start = entry->base;

        if (minimum_start < PMM_RESERVED_LOW_MEMORY) {
            minimum_start = PMM_RESERVED_LOW_MEMORY;
        }

        uint64_t region_start;

        if (!align_up(
                minimum_start,
                PMM_PAGE_SIZE,
                &region_start)) {
            continue;
        }

        if (region_start > region_end) {
            continue;
        }

        if (bitmap_storage_size > region_end - region_start) {
            continue;
        }

        bitmap_physical_address = region_start;

        return true;
    }

    return false;
}


// Initialize the physical memory manager.
bool pmm_init(
    struct limine_memmap_response *memory_map,
    uint64_t offset
) {
    if (initialized ||
        memory_map == 0 ||
        memory_map->entries == 0 ||
        memory_map->entry_count == 0 ||
        offset == 0) {
        return false;
    }

    hhdm_offset = offset;

    uint64_t highest_usable_address = 0;

    // Find the highest usable physical address.
    for (uint64_t i = 0; i < memory_map->entry_count; i++) {

        struct limine_memmap_entry *entry =
            memory_map->entries[i];

        if (entry == 0 ||
            entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        if (entry->length > UINT64_MAX - entry->base) {
            continue;
        }

        uint64_t region_end = entry->base + entry->length;

        if (region_end > highest_usable_address) {
            highest_usable_address = region_end;
        }
    }

    if (highest_usable_address == 0 ||
        highest_usable_address >
            UINT64_MAX - (PMM_PAGE_SIZE - 1)) {
        return false;
    }

    // Calculate the number of physical frames.
    total_frames =
        (highest_usable_address + PMM_PAGE_SIZE - 1)
        >> PMM_PAGE_SHIFT;

    // One bit per frame, rounded up to whole bytes.
    bitmap_size =
        (total_frames + PMM_BITS_PER_BYTE - 1)
        / PMM_BITS_PER_BYTE;

    // Two bitmaps, rounded up to complete physical pages.
    if (bitmap_size > (UINT64_MAX / 2)) {
        return false;
    }

    uint64_t both_bitmaps_size = bitmap_size * 2;

    if (!align_up(
            both_bitmaps_size,
            PMM_PAGE_SIZE,
            &bitmap_storage_size)) {
        return false;
    }

    // Find physical memory for the bitmaps.
    if (!find_bitmap_region(memory_map)) {
        return false;
    }

    // Convert the physical address into its HHDM virtual address.
    if (bitmap_physical_address >
        UINT64_MAX - hhdm_offset) {
        return false;
    }

    frame_bitmap = (uint8_t *)(uintptr_t)(
        hhdm_offset + bitmap_physical_address
    );

    allocation_bitmap = frame_bitmap + bitmap_size;

    // Initially, mark every frame as occupied.
    for (uint64_t i = 0; i < bitmap_size; i++) {
        frame_bitmap[i] = 0xFF;
        allocation_bitmap[i] = 0x00;
    }

    free_frames = 0;
    next_search_frame = 0;

    // Release only complete, page-aligned USABLE frames.
    for (uint64_t i = 0; i < memory_map->entry_count; i++) {

        struct limine_memmap_entry *entry =
            memory_map->entries[i];

        if (entry == 0 ||
            entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        if (entry->length > UINT64_MAX - entry->base) {
            continue;
        }

        uint64_t region_end = entry->base + entry->length;

        uint64_t start;

        if (!align_up(
                entry->base,
                PMM_PAGE_SIZE,
                &start)) {
            continue;
        }

        uint64_t end =
            region_end & ~(PMM_PAGE_SIZE - 1);

        for (uint64_t address = start;
             address < end;
             address += PMM_PAGE_SIZE) {

            // Keep the first 1 MiB reserved.
            if (address < PMM_RESERVED_LOW_MEMORY) {
                continue;
            }

            // Keep the bitmap's physical storage reserved.
            if (address >= bitmap_physical_address &&
                address - bitmap_physical_address <
                    bitmap_storage_size) {
                continue;
            }

            uint64_t frame = address >> PMM_PAGE_SHIFT;

            if (frame >= total_frames) {
                continue;
            }

            bitmap_clear(frame_bitmap, frame);
            free_frames++;
        }
    }

    initialized = true;

    return true;
}


/*
 * Convert an allocated physical frame into its HHDM virtual
 * address.
 *
 * INTERNAL FUNCTION:
 * The caller must already hold pmm_lock.
 */
static void *pmm_frame_to_virt_unlocked(
    uint64_t physical_address
) {

    if (!initialized ||
        physical_address == 0 ||
        (physical_address & (PMM_PAGE_SIZE - 1)) != 0) {

        return NULL;
    }

    uint64_t frame =
        physical_address >> PMM_PAGE_SHIFT;

    if (frame >= total_frames) {
        return NULL;
    }

    if (!bitmap_test(allocation_bitmap, frame)) {
        return NULL;
    }

    if (physical_address > UINT64_MAX - hhdm_offset) {
        return NULL;
    }

    return (void *)(uintptr_t)(
        hhdm_offset + physical_address
    );
}


/*
 * Allocate one physical frame using next-fit searching.
 *
 * The frame is marked allocated while holding the PMM lock.
 * Zero initialization happens after releasing the lock to
 * minimize the duration of the critical section.
 */
uint64_t pmm_alloc_frame(void) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return 0;
    }

    if (!initialized || free_frames == 0) {

        pmm_lock_release(irq_flags);
        return 0;
    }

    /*
     * Inject one allocation failure after the requested
     * number of successful frame allocations.
     */
    if (pmm_test_alloc_failure_countdown_enabled &&
        pmm_test_alloc_successes_remaining == 0) {

        pmm_test_alloc_failure_countdown_enabled = false;

        pmm_test_alloc_successes_remaining = 0;

        pmm_lock_release(irq_flags);

        return 0;
    }

    uint64_t frame = next_search_frame;

    /*
     * Examine every physical frame at most once.
     */
    for (uint64_t scanned = 0;
         scanned < total_frames;
         scanned++) {

        if (!bitmap_test(frame_bitmap, frame)) {

            /*
             * Reserve the selected frame.
             */
            bitmap_set(frame_bitmap, frame);
            bitmap_set(allocation_bitmap, frame);

            free_frames--;

            /*
             * Update the next-fit search position.
             */
            next_search_frame = frame + 1;

            if (next_search_frame >= total_frames) {
                next_search_frame = 0;
            }

            uint64_t physical_address =
                frame << PMM_PAGE_SHIFT;

            /*
             * Obtain the HHDM pointer without recursively
             * acquiring pmm_lock.
             */
            void *virtual_address =
                pmm_frame_to_virt_unlocked(
                    physical_address
                );

            if (virtual_address == NULL) {

                /*
                 * Restore the frame if conversion fails.
                 */
                bitmap_clear(allocation_bitmap, frame);
                bitmap_clear(frame_bitmap, frame);

                free_frames++;

                pmm_lock_release(irq_flags);

                return 0;
            }

            /*
             * Count this allocation only after the frame
             * has been reserved and its HHDM alias validated.
             */
            if (pmm_test_alloc_failure_countdown_enabled &&
                pmm_test_alloc_successes_remaining > 0) {

                pmm_test_alloc_successes_remaining--;
            }

            /*
             * The frame is now exclusively reserved.
             * Release the allocator lock before clearing
             * its 4096 bytes.
             */
            pmm_lock_release(irq_flags);

            memset(
                virtual_address,
                0,
                (size_t)PMM_PAGE_SIZE
            );

            return physical_address;
        }

        frame++;

        if (frame >= total_frames) {
            frame = 0;
        }
    }

    pmm_lock_release(irq_flags);

    return 0;
}

/*
 * Arm a one-shot allocation failure after N successful
 * physical-frame allocations.
 *
 * successful_allocations == 0 means the next allocation fails.
 */
bool pmm_test_fail_alloc_after(
    uint64_t successful_allocations
) {
    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return false;
    }

    if (!initialized) {
        pmm_lock_release(irq_flags);
        return false;
    }

    pmm_test_alloc_successes_remaining =
        successful_allocations;

    pmm_test_alloc_failure_countdown_enabled = true;

    pmm_lock_release(irq_flags);

    return true;
}


/*
 * Disable allocation failure injection.
 */
void pmm_test_clear_alloc_failure(void) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return;
    }

    pmm_test_alloc_failure_countdown_enabled = false;

    pmm_test_alloc_successes_remaining = 0;

    pmm_lock_release(irq_flags);
}

/*
 * Release a frame previously allocated by the PMM.
 */
bool pmm_free_frame(uint64_t physical_address) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return false;
    }

    if (!initialized ||
        physical_address == 0 ||
        (physical_address & (PMM_PAGE_SIZE - 1)) != 0) {

        pmm_lock_release(irq_flags);
        return false;
    }

    uint64_t frame =
        physical_address >> PMM_PAGE_SHIFT;

    if (frame >= total_frames) {

        pmm_lock_release(irq_flags);
        return false;
    }

    /*
     * Only frames allocated through pmm_alloc_frame()
     * may be released.
     */
    if (!bitmap_test(allocation_bitmap, frame)) {

        pmm_lock_release(irq_flags);
        return false;
    }

    /*
     * Inject one targeted release failure.
     *
     * The allocation bitmap and free-frame counter remain
     * unchanged. A later cleanup attempt can retry safely.
     */
    if (pmm_test_free_failure_physical_address ==
        physical_address) {

        pmm_test_last_failed_free_physical_address =
            physical_address;

        pmm_test_free_failure_physical_address = 0;

        pmm_lock_release(irq_flags);
        return false;
    }

    /*
    * Inject one countdown-based release failure.
    *
    * All normal validation has already succeeded. Therefore,
    * every permitted operation below this point can complete
    * as a successful frame release.
    */
    if (pmm_test_free_failure_countdown_enabled) {

        if (pmm_test_free_successes_remaining == 0) {

            pmm_test_last_failed_free_physical_address =
                physical_address;

            /*
            * Consume the one-shot failure.
            * The frame remains allocated and retryable.
            */
            pmm_test_free_failure_countdown_enabled = false;

            pmm_lock_release(irq_flags);
            return false;
        }

        pmm_test_free_successes_remaining--;
    }

    bitmap_clear(allocation_bitmap, frame);
    bitmap_clear(frame_bitmap, frame);

    free_frames++;

    pmm_lock_release(irq_flags);

    return true;
}

/*
 * Return the physical address of the most recent frame
 * whose release was rejected by a test failure hook.
 *
 * Zero means no injected release failure has occurred
 * since the last failure hook was armed.
 */
uint64_t pmm_test_get_last_failed_free_frame(void) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return 0;
    }

    uint64_t physical_address =
        pmm_test_last_failed_free_physical_address;

    pmm_lock_release(irq_flags);

    return physical_address;
}

/*
 * Convert an allocated physical frame into its HHDM
 * virtual address.
 */
void *pmm_frame_to_virt(uint64_t physical_address) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return NULL;
    }

    void *virtual_address =
        pmm_frame_to_virt_unlocked(physical_address);

    pmm_lock_release(irq_flags);

    return virtual_address;
}


/*
 * Return the number of currently available physical frames.
 */
uint64_t pmm_get_free_frames(void) {

    uint64_t irq_flags = 0;

    if (!pmm_lock_acquire(&irq_flags)) {
        return 0;
    }

    uint64_t count =
        initialized ? free_frames : 0;

    pmm_lock_release(irq_flags);

    return count;
}
