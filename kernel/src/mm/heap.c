
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#include "arch/x86_64/irq_flags.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"

#define HEAP_PAGE_SIZE       PMM_PAGE_SIZE

#define HEAP_ARENA_BASE      0xFFFFFF0000000000ULL
#define HEAP_ARENA_SIZE      (16ULL * 1024ULL * 1024ULL)
#define HEAP_PAGE_COUNT      (HEAP_ARENA_SIZE / HEAP_PAGE_SIZE)

#define HEAP_MAX_ALLOCATIONS 256ULL

#define HEAP_SLAB_CLASS_COUNT 7ULL
#define HEAP_SLAB_MAX_OBJECT_SIZE 1024ULL
#define HEAP_SLAB_MIN_OBJECT_SIZE 16ULL

#define HEAP_SLAB_MAX_OBJECTS \
    (HEAP_PAGE_SIZE / HEAP_SLAB_MIN_OBJECT_SIZE)

#define HEAP_SLAB_BITMAP_WORDS \
    ((HEAP_SLAB_MAX_OBJECTS + 63ULL) / 64ULL)

#define HEAP_MAX_SLABS 256ULL
#define HEAP_MAX_NAMED_CACHES 32ULL
#define HEAP_CACHE_NAME_LENGTH 32ULL

/*
 * Heap operation protection.
 *
 * Before scheduler initialization:
 *     Interrupts are disabled because the kernel is still
 *     executing in its single-threaded bootstrap phase.
 *
 * After scheduler initialization:
 *     Involuntary preemption is disabled for the current
 *     thread using the scheduler's nested preemption counter.
 *
 * This protects the global heap metadata against concurrent
 * execution by another thread on CPU0.
 *
 * This is NOT an SMP lock.
 */

struct heap_operation_guard {

    bool uses_preemption;

    bool active;

    uint64_t irq_flags;
};


/*
 * Enter a protected heap operation.
 *
 * Nested heap API calls are supported because the scheduler's
 * preemption counter supports nesting.
 */
static bool heap_guard_enter(
    struct heap_operation_guard *guard
) {

    if (guard == NULL) {
        return false;
    }

    guard->uses_preemption = false;
    guard->active = false;
    guard->irq_flags = 0;

    /*
     * The scheduler is not yet available during early boot.
     *
     * Protect bootstrap heap operations by disabling interrupts.
     */
    if (!scheduler_is_initialized()) {

        guard->irq_flags = arch_irq_save_disable();

        guard->uses_preemption = false;
        guard->active = true;

        return true;
    }

    /*
     * Once scheduling is active, protect the current thread
     * against involuntary context switches.
     */
    if (!thread_preempt_disable()) {
        return false;
    }

    guard->uses_preemption = true;
    guard->active = true;

    return true;
}


/*
 * Leave a protected heap operation.
 */
static void heap_guard_leave(
    struct heap_operation_guard *guard
) {

    if (guard == NULL || !guard->active) {
        return;
    }

    guard->active = false;

    if (guard->uses_preemption) {

        /*
         * Release exactly one nesting level.
         */
        if (!thread_preempt_enable()) {

            /*
             * A failed release indicates corrupted scheduler
             * preemption state. Continuing could corrupt memory.
             */
            asm volatile ("cli" ::: "memory");

            for (;;) {
                asm volatile ("hlt");
            }
        }

        return;
    }

    /*
     * Restore the interrupt state that existed before
     * entering the bootstrap heap operation.
     */
    arch_irq_restore(guard->irq_flags);
}

struct heap_cache {
    char name[HEAP_CACHE_NAME_LENGTH];

    uint64_t object_size;
    uint64_t active_objects;
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t slab_count;

    heap_cache_constructor_t constructor;
    heap_cache_destructor_t destructor;

    bool active;
};

/*
 * Page-backed allocation metadata.
 *
 * Used for large allocations and slab backing pages.
 */
typedef struct {
    uint64_t start_page;
    uint64_t page_count;
    uint64_t requested_size;
    bool active;
} heap_allocation_t;


/*
 * One slab descriptor represents one 4 KiB slab page.
 *
 * The allocation bitmap tracks which objects are occupied.
 * Metadata is stored separately from the slab page.
 */
typedef struct {
    uintptr_t page_address;

    uint64_t object_size;
    uint64_t object_capacity;
    uint64_t allocated_count;

    uint64_t allocation_bitmap[HEAP_SLAB_BITMAP_WORDS];
    uint16_t requested_sizes[HEAP_SLAB_MAX_OBJECTS];

    /*
     * NULL means ordinary kmalloc slab.
     * Non-NULL identifies its named object cache.
     */
    heap_cache_t *owner;

    bool active;
} heap_slab_t;

heap_cache_t *owner;


/*
 * Size classes, ordered from smallest to largest.
 */
static const uint64_t heap_size_classes[
    HEAP_SLAB_CLASS_COUNT
] = {
    16,
    32,
    64,
    128,
    256,
    512,
    1024
};


/*
 * Global heap metadata.
 */
static heap_cache_t heap_caches[HEAP_MAX_NAMED_CACHES];
static uint8_t heap_page_used[HEAP_PAGE_COUNT];

static heap_allocation_t heap_allocations[
    HEAP_MAX_ALLOCATIONS
];

static heap_slab_t heap_slabs[
    HEAP_MAX_SLABS
];

static bool heap_initialized = false;


_Static_assert(
    HEAP_ARENA_SIZE % HEAP_PAGE_SIZE == 0,
    "Heap arena must contain whole pages"
);

_Static_assert(
    HEAP_SLAB_MAX_OBJECTS % 64ULL == 0,
    "Slab bitmap must contain complete words"
);


/*
 * Convert a heap page index into a virtual address.
 */
static uint64_t heap_page_address(uint64_t page_index) {

    return HEAP_ARENA_BASE +
           (page_index * HEAP_PAGE_SIZE);
}


/*
 * Find an unused page-allocation record.
 */
static heap_allocation_t *heap_find_free_record(void) {

    for (uint64_t i = 0;
         i < HEAP_MAX_ALLOCATIONS;
         i++) {

        if (!heap_allocations[i].active) {
            return &heap_allocations[i];
        }
    }

    return NULL;
}


/*
 * Find an unused slab descriptor.
 */
static heap_slab_t *heap_find_free_slab_descriptor(void) {

    for (uint64_t i = 0;
         i < HEAP_MAX_SLABS;
         i++) {

        if (!heap_slabs[i].active) {
            return &heap_slabs[i];
        }
    }

    return NULL;
}


/*
 * Find a contiguous range of unused virtual pages.
 */
static bool heap_find_free_pages(
    uint64_t page_count,
    uint64_t *start_page_out
) {

    if (page_count == 0 ||
        page_count > HEAP_PAGE_COUNT ||
        start_page_out == NULL) {

        return false;
    }

    for (uint64_t start = 0;
         start <= HEAP_PAGE_COUNT - page_count;
         start++) {

        bool available = true;

        for (uint64_t offset = 0;
             offset < page_count;
             offset++) {

            if (heap_page_used[start + offset]) {
                available = false;
                break;
            }
        }

        if (available) {
            *start_page_out = start;
            return true;
        }
    }

    return false;
}


/*
 * Mark or release a virtual page range.
 */
static void heap_set_pages_used(
    uint64_t start_page,
    uint64_t page_count,
    bool used
) {

    for (uint64_t i = 0; i < page_count; i++) {

        heap_page_used[start_page + i] =
            used ? 1 : 0;
    }
}


/*
 * Roll back mappings created during an allocation failure.
 */
static bool heap_rollback_mappings(
    uint64_t start_page,
    uint64_t mapped_count
) {

    bool success = true;

    while (mapped_count > 0) {

        mapped_count--;

        uint64_t virtual_address =
            heap_page_address(start_page + mapped_count);

        uint64_t physical_address = 0;

        vmm_status_t status = vmm_unmap_page(
            virtual_address,
            &physical_address
        );

        if (status != VMM_STATUS_OK) {
            success = false;
            continue;
        }

        if (!pmm_free_frame(physical_address)) {
            success = false;
        }
    }

    return success;
}


/*
 * Validate the candidate virtual arena.
 */
static bool heap_validate_arena(void) {

    vmm_mapping_info_t mapping;

    for (uint64_t i = 0;
         i < HEAP_PAGE_COUNT;
         i++) {

        uint64_t virtual_address =
            heap_page_address(i);

        vmm_status_t status = vmm_query(
            virtual_address,
            &mapping
        );

        if (status != VMM_STATUS_NOT_MAPPED) {
            return false;
        }
    }

    return true;
}


/*
 * Internal page-backed allocation function.
 *
 * This is the backing allocator used by:
 * - Large kmalloc requests.
 * - New slab pages.
 *
 * It deliberately bypasses the slab allocator.
 */
static void *heap_page_allocate(size_t size) {

    if (!heap_initialized || size == 0) {
        return NULL;
    }

    uint64_t requested_size = (uint64_t)size;

    uint64_t page_count =
        requested_size / HEAP_PAGE_SIZE;

    if (requested_size % HEAP_PAGE_SIZE != 0) {
        page_count++;
    }

    if (page_count == 0 ||
        page_count > HEAP_PAGE_COUNT) {

        return NULL;
    }

    heap_allocation_t *record =
        heap_find_free_record();

    if (record == NULL) {
        return NULL;
    }

    uint64_t start_page = 0;

    if (!heap_find_free_pages(
            page_count,
            &start_page
        )) {

        return NULL;
    }

    heap_set_pages_used(
        start_page,
        page_count,
        true
    );

    uint64_t mapped_count = 0;
    bool allocation_success = true;

    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t physical_address =
            pmm_alloc_frame();

        if (physical_address == 0) {
            allocation_success = false;
            break;
        }

        uint64_t virtual_address =
            heap_page_address(start_page + i);

        vmm_status_t status = vmm_map_page(
            virtual_address,
            physical_address,
            VMM_MAP_WRITABLE
        );

        if (status != VMM_STATUS_OK) {

            if (!pmm_free_frame(physical_address)) {
                allocation_success = false;
            }

            allocation_success = false;
            break;
        }

        mapped_count++;
    }

    if (!allocation_success) {

        bool rollback_success = heap_rollback_mappings(
            start_page,
            mapped_count
        );

        if (rollback_success) {

            heap_set_pages_used(
                start_page,
                page_count,
                false
            );
        }

        return NULL;
    }

    record->start_page = start_page;
    record->page_count = page_count;
    record->requested_size = requested_size;
    record->active = true;

    return (void *)heap_page_address(start_page);
}


/*
 * Internal page-backed release function.
 *
 * Only exact allocation-base pointers are accepted.
 */
static void heap_page_release(void *ptr) {

    if (!heap_initialized || ptr == NULL) {
        return;
    }

    uint64_t address = (uint64_t)(uintptr_t)ptr;

    heap_allocation_t *record = NULL;

    for (uint64_t i = 0;
         i < HEAP_MAX_ALLOCATIONS;
         i++) {

        if (heap_allocations[i].active &&
            heap_page_address(
                heap_allocations[i].start_page
            ) == address) {

            record = &heap_allocations[i];
            break;
        }
    }

    if (record == NULL) {
        return;
    }

    /*
     * Preflight every page before changing any mapping.
     */
    for (uint64_t i = 0;
         i < record->page_count;
         i++) {

        uint64_t virtual_address =
            heap_page_address(record->start_page + i);

        vmm_mapping_info_t mapping;

        vmm_status_t status = vmm_query(
            virtual_address,
            &mapping
        );

        if (status != VMM_STATUS_OK ||
            mapping.page_size != HEAP_PAGE_SIZE ||
            mapping.huge_page ||
            (mapping.physical_address &
                (HEAP_PAGE_SIZE - 1)) != 0) {

            return;
        }
    }

    /*
     * Unmap pages and return their physical frames.
     */
    for (uint64_t i = 0;
         i < record->page_count;
         i++) {

        uint64_t virtual_address =
            heap_page_address(record->start_page + i);

        uint64_t physical_address = 0;

        vmm_status_t status = vmm_unmap_page(
            virtual_address,
            &physical_address
        );

        if (status != VMM_STATUS_OK) {
            return;
        }

        if (!pmm_free_frame(physical_address)) {
            return;
        }
    }

    heap_set_pages_used(
        record->start_page,
        record->page_count,
        false
    );

    record->start_page = 0;
    record->page_count = 0;
    record->requested_size = 0;
    record->active = false;
}


/*
 * Find the appropriate size class.
 *
 * Returns HEAP_SLAB_CLASS_COUNT if the request is too large.
 */
static uint64_t heap_get_size_class(size_t size) {

    for (uint64_t i = 0;
         i < HEAP_SLAB_CLASS_COUNT;
         i++) {

        if ((uint64_t)size <= heap_size_classes[i]) {
            return i;
        }
    }

    return HEAP_SLAB_CLASS_COUNT;
}


/*
 * Find a free object slot in a slab.
 *
 * Returns false if the slab has no free objects.
 */
static bool heap_slab_find_free_object(
    heap_slab_t *slab,
    uint64_t *object_index_out
) {

    if (slab == NULL ||
        object_index_out == NULL ||
        !slab->active) {

        return false;
    }

    for (uint64_t i = 0;
         i < slab->object_capacity;
         i++) {

        uint64_t word_index = i / 64ULL;
        uint64_t bit_index = i % 64ULL;

        uint64_t mask = 1ULL << bit_index;

        if (!(slab->allocation_bitmap[word_index] & mask)) {

            *object_index_out = i;
            return true;
        }
    }

    return false;
}


/*
 * Mark a slab object allocated or free.
 */
static void heap_slab_set_object(
    heap_slab_t *slab,
    uint64_t object_index,
    bool allocated
) {

    uint64_t word_index = object_index / 64ULL;
    uint64_t bit_index = object_index % 64ULL;

    uint64_t mask = 1ULL << bit_index;

    if (allocated) {

        slab->allocation_bitmap[word_index] |= mask;
        slab->allocated_count++;

    } else {

        slab->allocation_bitmap[word_index] &= ~mask;

        if (slab->allocated_count > 0) {
            slab->allocated_count--;
        }
    }
}


/*
 * Create a new slab for a particular size class.
 */

static heap_slab_t *heap_create_slab(
    uint64_t object_size,
    heap_cache_t *owner
) {
    heap_slab_t *slab =
        heap_find_free_slab_descriptor();

    if (slab == NULL) {
        return NULL;
    }

    void *page = heap_page_allocate(HEAP_PAGE_SIZE);

    if (page == NULL) {
        return NULL;
    }

    slab->page_address = (uintptr_t)page;
    slab->object_size = object_size;
    slab->object_capacity = HEAP_PAGE_SIZE / object_size;
    slab->allocated_count = 0;
    slab->owner = owner;

    for (uint64_t i = 0;
         i < HEAP_SLAB_BITMAP_WORDS;
         i++) {
        slab->allocation_bitmap[i] = 0;
    }

    for (uint64_t i = 0;
         i < HEAP_SLAB_MAX_OBJECTS;
         i++) {

        slab->requested_sizes[i] = 0;
    }

    slab->active = true;

    if (owner != NULL) {
        owner->slab_count++;
    }

    return slab;
}



/*
 * Allocate an object from a slab cache.
 */

static void *heap_slab_allocate(
    uint64_t object_size,
    uint64_t requested_size,
    heap_cache_t *owner
) {
    if (object_size < HEAP_SLAB_MIN_OBJECT_SIZE ||
        object_size > HEAP_SLAB_MAX_OBJECT_SIZE ||
        (object_size & 15ULL) != 0 ||
        requested_size == 0 ||
        requested_size > object_size) {

        return NULL;
    }

    heap_slab_t *selected_slab = NULL;
    uint64_t object_index = 0;

    for (uint64_t i = 0;
         i < HEAP_MAX_SLABS;
         i++) {
        heap_slab_t *slab = &heap_slabs[i];

        if (!slab->active ||
            slab->owner != owner ||
            slab->object_size != object_size ||
            slab->allocated_count >= slab->object_capacity) {
            continue;
        }

        if (heap_slab_find_free_object(slab, &object_index)) {
            selected_slab = slab;
            break;
        }
    }

    if (selected_slab == NULL) {
        selected_slab = heap_create_slab(object_size, owner);

        if (selected_slab == NULL) {
            return NULL;
        }

        if (!heap_slab_find_free_object(
                selected_slab,
                &object_index)) {

            heap_page_release(
                (void *)selected_slab->page_address
            );

            bool page_still_allocated = false;

            for (uint64_t i = 0;
                 i < HEAP_MAX_ALLOCATIONS;
                 i++) {
                if (heap_allocations[i].active &&
                    heap_page_address(
                        heap_allocations[i].start_page
                    ) == selected_slab->page_address) {
                    page_still_allocated = true;
                    break;
                }
            }

            if (!page_still_allocated) {
                selected_slab->active = false;

                if (owner != NULL && owner->slab_count > 0) {
                    owner->slab_count--;
                }
            }

            return NULL;
        }
    }

    heap_slab_set_object(
        selected_slab,
        object_index,
        true
    );

    selected_slab->requested_sizes[object_index] =
        (uint16_t)requested_size;

    uintptr_t object_address =
        selected_slab->page_address +
        (object_index * object_size);

    return (void *)object_address;
}



/*
 * Find the slab that contains a pointer.
 */
static heap_slab_t *heap_find_slab(
    uintptr_t address
) {

    for (uint64_t i = 0;
         i < HEAP_MAX_SLABS;
         i++) {

        heap_slab_t *slab = &heap_slabs[i];

        if (!slab->active) {
            continue;
        }

        if (address >= slab->page_address &&
            address < slab->page_address + HEAP_PAGE_SIZE) {

            return slab;
        }
    }

    return NULL;
}


/*
 * Free an object from a slab.
 */

static bool heap_slab_release(
    heap_slab_t *slab,
    uintptr_t address,
    heap_cache_t *expected_owner
) {
    if (slab == NULL ||
        !slab->active ||
        slab->owner != expected_owner ||
        address < slab->page_address) {
        return false;
    }

    uint64_t offset = address - slab->page_address;

    if (offset % slab->object_size != 0) {
        return false;
    }

    uint64_t object_index = offset / slab->object_size;

    if (object_index >= slab->object_capacity) {
        return false;
    }

    uint64_t word_index = object_index / 64ULL;
    uint64_t bit_index = object_index % 64ULL;
    uint64_t mask = 1ULL << bit_index;

    if (!(slab->allocation_bitmap[word_index] & mask)) {
        return false;
    }

    heap_slab_set_object(slab, object_index, false);

    slab->requested_sizes[object_index] = 0;

    if (slab->allocated_count == 0) {
        uintptr_t page_address = slab->page_address;

        heap_page_release((void *)page_address);

        bool page_still_allocated = false;

        for (uint64_t i = 0;
             i < HEAP_MAX_ALLOCATIONS;
             i++) {
            if (heap_allocations[i].active &&
                heap_page_address(
                    heap_allocations[i].start_page
                ) == page_address) {
                page_still_allocated = true;
                break;
            }
        }

        if (!page_still_allocated) {
            slab->page_address = 0;
            slab->object_size = 0;
            slab->object_capacity = 0;
            slab->allocated_count = 0;
            slab->owner = NULL;

            for (uint64_t i = 0;
                 i < HEAP_SLAB_BITMAP_WORDS;
                 i++) {
                slab->allocation_bitmap[i] = 0;
            }

            for (uint64_t i = 0;
                i < HEAP_SLAB_MAX_OBJECTS;
                i++) {

                slab->requested_sizes[i] = 0;
            }

            slab->active = false;

            if (expected_owner != NULL &&
                expected_owner->slab_count > 0) {
                expected_owner->slab_count--;
            }
        }
    }

    return true;
}


/*
 * Validate a cache handle against our static descriptor table.
 */
static bool heap_cache_is_valid(const heap_cache_t *cache) {
    if (cache == NULL) {
        return false;
    }

    for (uint64_t i = 0;
         i < HEAP_MAX_NAMED_CACHES;
         i++) {
        if (cache == &heap_caches[i]) {
            return cache->active;
        }
    }

    return false;
}


/*
 * Find a slab object and verify that it is currently allocated.
 */
static bool heap_cache_validate_object(
    heap_cache_t *cache,
    void *ptr,
    heap_slab_t **slab_out
) {
    if (!heap_cache_is_valid(cache) ||
        ptr == NULL ||
        slab_out == NULL) {
        return false;
    }

    uintptr_t address = (uintptr_t)ptr;
    heap_slab_t *slab = heap_find_slab(address);

    if (slab == NULL ||
        slab->owner != cache ||
        address < slab->page_address) {
        return false;
    }

    uint64_t offset = address - slab->page_address;

    if (offset % slab->object_size != 0) {
        return false;
    }

    uint64_t object_index = offset / slab->object_size;

    if (object_index >= slab->object_capacity) {
        return false;
    }

    uint64_t word_index = object_index / 64ULL;
    uint64_t bit_index = object_index % 64ULL;
    uint64_t mask = 1ULL << bit_index;

    if (!(slab->allocation_bitmap[word_index] & mask)) {
        return false;
    }

    *slab_out = slab;
    return true;
}


/*
 * Create a named fixed-size object cache.
 */
static heap_cache_t *heap_cache_create_unprotected(
    const char *name,
    size_t object_size,
    heap_cache_constructor_t constructor,
    heap_cache_destructor_t destructor
) {
    if (!heap_initialized ||
        name == NULL ||
        object_size == 0 ||
        object_size > HEAP_SLAB_MAX_OBJECT_SIZE) {
        return NULL;
    }

    uint64_t name_length = 0;

    while (name[name_length] != '\0') {
        name_length++;

        if (name_length >= HEAP_CACHE_NAME_LENGTH) {
            return NULL;
        }
    }

    if (name_length == 0) {
        return NULL;
    }

    for (uint64_t i = 0;
         i < HEAP_MAX_NAMED_CACHES;
         i++) {
        if (heap_caches[i].active) {
            uint64_t j = 0;

            while (j < HEAP_CACHE_NAME_LENGTH &&
                   heap_caches[i].name[j] == name[j] &&
                   name[j] != '\0') {
                j++;
            }

            if (j == name_length &&
                heap_caches[i].name[j] == '\0') {
                return NULL;
            }
        }
    }

    heap_cache_t *cache = NULL;

    for (uint64_t i = 0;
         i < HEAP_MAX_NAMED_CACHES;
         i++) {
        if (!heap_caches[i].active) {
            cache = &heap_caches[i];
            break;
        }
    }

    if (cache == NULL) {
        return NULL;
    }

    uint64_t aligned_size =
        ((uint64_t)object_size + 15ULL) & ~15ULL;

    if (aligned_size > HEAP_SLAB_MAX_OBJECT_SIZE) {
        return NULL;
    }

    for (uint64_t i = 0; i < HEAP_CACHE_NAME_LENGTH; i++) {
        cache->name[i] = (i < name_length) ? name[i] : '\0';
    }

    cache->object_size = aligned_size;
    cache->active_objects = 0;
    cache->total_allocations = 0;
    cache->total_frees = 0;
    cache->slab_count = 0;

    cache->constructor = constructor;
    cache->destructor = destructor;

    cache->active = true;

    return cache;
}


/*
 * Allocate and initialize one object from a named cache.
 */
static void *heap_cache_alloc_unprotected(heap_cache_t *cache) {
    if (!heap_cache_is_valid(cache)) {
        return NULL;
    }

    void *object = heap_slab_allocate(
        cache->object_size,
        cache->object_size,
        cache
    );

    if (object == NULL) {
        return NULL;
    }

    if (cache->constructor != NULL &&
        !cache->constructor(object)) {

        heap_slab_t *slab = heap_find_slab(
            (uintptr_t)object
        );

        heap_slab_release(
            slab,
            (uintptr_t)object,
            cache
        );

        return NULL;
    }

    cache->active_objects++;
    cache->total_allocations++;

    return object;
}


/*
 * Destroy one allocated object and return it to its owning cache.
 */
static bool heap_cache_free_unprotected(heap_cache_t *cache, void *ptr) {
    heap_slab_t *slab = NULL;

    if (!heap_cache_validate_object(cache, ptr, &slab)) {
        return false;
    }

    if (cache->destructor != NULL) {
        cache->destructor(ptr);
    }

    if (!heap_slab_release(
            slab,
            (uintptr_t)ptr,
            cache)) {
        return false;
    }

    if (cache->active_objects > 0) {
        cache->active_objects--;
    }

    cache->total_frees++;

    return true;
}


/*
 * Destroy a cache only when no objects remain allocated.
 */
static bool heap_cache_destroy_unprotected(heap_cache_t *cache) {
    if (!heap_cache_is_valid(cache) ||
        cache->active_objects != 0) {
        return false;
    }

    for (uint64_t i = 0;
         i < HEAP_MAX_SLABS;
         i++) {
        heap_slab_t *slab = &heap_slabs[i];

        if (!slab->active || slab->owner != cache) {
            continue;
        }

        if (slab->allocated_count != 0) {
            return false;
        }

        uintptr_t page_address = slab->page_address;

        heap_page_release((void *)page_address);

        bool page_still_allocated = false;

        for (uint64_t j = 0;
             j < HEAP_MAX_ALLOCATIONS;
             j++) {
            if (heap_allocations[j].active &&
                heap_page_address(
                    heap_allocations[j].start_page
                ) == page_address) {
                page_still_allocated = true;
                break;
            }
        }

        if (page_still_allocated) {
            return false;
        }

        slab->page_address = 0;
        slab->object_size = 0;
        slab->object_capacity = 0;
        slab->allocated_count = 0;
        slab->owner = NULL;

        for (uint64_t j = 0;
             j < HEAP_SLAB_BITMAP_WORDS;
             j++) {
            slab->allocation_bitmap[j] = 0;
        }

        slab->active = false;

        if (cache->slab_count > 0) {
            cache->slab_count--;
        }
    }

    if (cache->slab_count != 0) {
        return false;
    }

    cache->active = false;
    return true;
}


/*
 * Read named-cache statistics.
 */
static bool heap_cache_get_stats_unprotected(
    const heap_cache_t *cache,
    heap_cache_stats_t *stats
) {
    if (!heap_cache_is_valid(cache) ||
        stats == NULL) {
        return false;
    }

    stats->object_size = (size_t)cache->object_size;
    stats->active_objects = cache->active_objects;
    stats->total_allocations = cache->total_allocations;
    stats->total_frees = cache->total_frees;
    stats->slab_count = cache->slab_count;

    return true;
}


/*
 * Initialize the kernel heap.
 */
static bool heap_init_unprotected(void) {

    if (heap_initialized) {
        return false;
    }

    if (!vmm_nx_is_enabled()) {
        return false;
    }

    if (!heap_validate_arena()) {
        return false;
    }

    heap_initialized = true;

    return true;
}


/*
 * Public allocation API.
 *
 * Small requests use slab caches.
 * Larger requests use the page-backed allocator.
 */
static void *heap_kmalloc_unprotected(size_t size) {

    if (!heap_initialized || size == 0) {
        return NULL;
    }

    uint64_t class_index =
        heap_get_size_class(size);

    if (class_index < HEAP_SLAB_CLASS_COUNT) {
        return heap_slab_allocate(
            heap_size_classes[class_index],
            size,
            NULL
        );
    }

    return heap_page_allocate(size);
}


/*
 * Public release API.
 *
 * First check whether the pointer belongs to a slab.
 * Otherwise, try the page-backed allocation records.
 */
static void heap_kfree_unprotected(void *ptr) {

    if (!heap_initialized || ptr == NULL) {
        return;
    }

    uintptr_t address = (uintptr_t)ptr;

    heap_slab_t *slab =
        heap_find_slab(address);

    if (slab != NULL) {

        heap_slab_release(
            slab,
            address,
            NULL
        );

        return;
    }

    heap_page_release(ptr);
}


/*
 * Allocate an array and initialize every byte to zero.
 *
 * Rejects multiplication overflow before allocating.
 */
static void *heap_kcalloc_unprotected(size_t count, size_t size) {

    if (count == 0 || size == 0) {
        return NULL;
    }

    if (count > SIZE_MAX / size) {
        return NULL;
    }

    size_t total_size = count * size;

    void *ptr = kmalloc(total_size);

    if (ptr == NULL) {
        return NULL;
    }

    uint8_t *bytes = (uint8_t *)ptr;

    for (size_t i = 0; i < total_size; i++) {
        bytes[i] = 0;
    }

    return ptr;
}


/*
 * Resize an existing ordinary kernel heap allocation.
 *
 * Named-cache objects cannot be resized with krealloc().
 */
static void *heap_krealloc_unprotected(void *ptr, size_t new_size) {

    if (ptr == NULL) {
        return kmalloc(new_size);
    }

    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    if (!heap_initialized) {
        return NULL;
    }

    uintptr_t address = (uintptr_t)ptr;

    uint64_t old_size = 0;
    uint64_t capacity = 0;

    heap_slab_t *slab = heap_find_slab(address);
    heap_allocation_t *page_record = NULL;

    uint64_t slab_object_index = 0;

    /*
     * Case 1: The pointer belongs to a slab.
     */
    if (slab != NULL) {

        /*
         * Named-cache objects have their own
         * allocation and destruction API.
         */
        if (slab->owner != NULL) {
            return NULL;
        }

        uint64_t offset =
            address - slab->page_address;

        if (offset % slab->object_size != 0) {
            return NULL;
        }

        slab_object_index =
            offset / slab->object_size;

        if (slab_object_index >= slab->object_capacity) {
            return NULL;
        }

        uint64_t word_index =
            slab_object_index / 64ULL;

        uint64_t bit_index =
            slab_object_index % 64ULL;

        uint64_t mask = 1ULL << bit_index;

        if (!(slab->allocation_bitmap[word_index] & mask)) {
            return NULL;
        }

        old_size =
            slab->requested_sizes[slab_object_index];

        capacity = slab->object_size;

        if (old_size == 0) {
            return NULL;
        }

        /*
         * The existing slot can hold the new request.
         * Keep the same address and update its metadata.
         */
        if (new_size <= capacity) {

            slab->requested_sizes[slab_object_index] =
                (uint16_t)new_size;

            return ptr;
        }
    }

    /*
     * Case 2: The pointer belongs to a page-backed
     * allocation. Only exact allocation-base pointers
     * are accepted.
     */
    else {

        for (uint64_t i = 0;
             i < HEAP_MAX_ALLOCATIONS;
             i++) {

            if (!heap_allocations[i].active) {
                continue;
            }

            if (heap_page_address(
                    heap_allocations[i].start_page
                ) == address) {

                page_record = &heap_allocations[i];
                break;
            }
        }

        if (page_record == NULL) {
            return NULL;
        }

        old_size = page_record->requested_size;

        capacity =
            page_record->page_count * HEAP_PAGE_SIZE;

        if (old_size == 0) {
            return NULL;
        }

        /*
         * Existing mapped pages can hold the new request.
         */
        if (new_size <= capacity) {

            page_record->requested_size = new_size;

            return ptr;
        }
    }

    /*
     * The existing allocation is too small.
     *
     * Allocate the replacement first. If allocation fails,
     * the original allocation remains untouched.
     */
    void *new_ptr = kmalloc(new_size);

    if (new_ptr == NULL) {
        return NULL;
    }

    /*
     * Copy only the original requested data.
     * Never copy the entire rounded-up slab capacity.
     */
    size_t copy_size =
        old_size < new_size ? (size_t)old_size : new_size;

    uint8_t *destination = (uint8_t *)new_ptr;
    const uint8_t *source = (const uint8_t *)ptr;

    for (size_t i = 0; i < copy_size; i++) {
        destination[i] = source[i];
    }

    /*
     * Release the old allocation only after the replacement
     * has been allocated and its data copied.
     */
    kfree(ptr);

    return new_ptr;
}

/*
 * ============================================================
 * Protected public heap API
 * ============================================================
 *
 * Every public heap operation enters a protection guard.
 *
 * Nested public operations are supported.
 */


/*
 * Initialize the kernel heap.
 */
bool heap_init(void) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return false;
    }

    bool result = heap_init_unprotected();

    heap_guard_leave(&guard);

    return result;
}


/*
 * Allocate memory.
 */
void *kmalloc(size_t size) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return NULL;
    }

    void *result = heap_kmalloc_unprotected(size);

    heap_guard_leave(&guard);

    return result;
}


/*
 * Free memory.
 */
void kfree(void *ptr) {

    if (ptr == NULL) {
        return;
    }

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return;
    }

    heap_kfree_unprotected(ptr);

    heap_guard_leave(&guard);
}


/*
 * Allocate zero-initialized memory.
 */
void *kcalloc(
    size_t count,
    size_t size
) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return NULL;
    }

    void *result = heap_kcalloc_unprotected(
        count,
        size
    );

    heap_guard_leave(&guard);

    return result;
}


/*
 * Resize an allocation.
 */
void *krealloc(
    void *ptr,
    size_t new_size
) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return NULL;
    }

    void *result = heap_krealloc_unprotected(
        ptr,
        new_size
    );

    heap_guard_leave(&guard);

    return result;
}


/*
 * Create a named object cache.
 */
heap_cache_t *heap_cache_create(
    const char *name,
    size_t object_size,
    heap_cache_constructor_t constructor,
    heap_cache_destructor_t destructor
) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return NULL;
    }

    heap_cache_t *result = heap_cache_create_unprotected(
        name,
        object_size,
        constructor,
        destructor
    );

    heap_guard_leave(&guard);

    return result;
}


/*
 * Allocate one object from a named cache.
 */
void *heap_cache_alloc(heap_cache_t *cache) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return NULL;
    }

    void *result = heap_cache_alloc_unprotected(cache);

    heap_guard_leave(&guard);

    return result;
}


/*
 * Free one object from a named cache.
 */
bool heap_cache_free(
    heap_cache_t *cache,
    void *ptr
) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return false;
    }

    bool result = heap_cache_free_unprotected(
        cache,
        ptr
    );

    heap_guard_leave(&guard);

    return result;
}


/*
 * Destroy an empty named cache.
 */
bool heap_cache_destroy(heap_cache_t *cache) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return false;
    }

    bool result = heap_cache_destroy_unprotected(cache);

    heap_guard_leave(&guard);

    return result;
}


/*
 * Read named-cache statistics.
 */
bool heap_cache_get_stats(
    const heap_cache_t *cache,
    heap_cache_stats_t *stats
) {

    struct heap_operation_guard guard;

    if (!heap_guard_enter(&guard)) {
        return false;
    }

    bool result = heap_cache_get_stats_unprotected(
        cache,
        stats
    );

    heap_guard_leave(&guard);

    return result;
}
