
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "drivers/console/console.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "heap_tests.h"

#define HEAP_TEST_PAGE_SIZE 4096ULL
#define HEAP_TEST_SLAB_OBJECTS 257ULL

typedef struct {
    uint64_t magic;
    uint64_t value;
} heap_test_object_t;

static uint64_t heap_test_constructor_calls = 0;
static uint64_t heap_test_destructor_calls = 0;

#define HEAP_TEST_OBJECT_MAGIC 0x4855414F424A4543ULL

static bool heap_test_object_constructor(void *object) {
    heap_test_object_t *test_object =
        (heap_test_object_t *)object;

    test_object->magic = HEAP_TEST_OBJECT_MAGIC;
    test_object->value = 0;

    heap_test_constructor_calls++;

    return true;
}

static void heap_test_object_destructor(void *object) {
    heap_test_object_t *test_object =
        (heap_test_object_t *)object;

    test_object->magic = 0;
    test_object->value = 0;

    heap_test_destructor_calls++;
}


static bool heap_test_failure(const char *message) {

    console_set_color(0xFF5555, 0x101820);

    console_write("[ FAILED ] ");
    console_write(message);
    console_write("\n");

    console_set_color(0xFFFFFF, 0x101820);

    return false;
}


static void heap_test_success(const char *message) {

    console_set_color(0x36D399, 0x101820);

    console_write("[ OK ] ");
    console_write(message);
    console_write("\n");

    console_set_color(0xFFFFFF, 0x101820);
}


bool heap_run_tests(void) {

    console_write("\n");
    console_write("===== KERNEL HEAP TESTS =====\n");

    uint64_t frames_before =
        pmm_get_free_frames();


    /*
     * TEST 1:
     * Zero-sized allocation rejection.
     */
    if (kmalloc(0) != NULL) {

        return heap_test_failure(
            "Zero-sized allocation rejection"
        );
    }

    heap_test_success(
        "Zero-sized allocation rejection"
    );


    /*
     * TEST 2:
     * Allocate multiple objects from the 16-byte cache.
     */
    void *small_a = kmalloc(1);
    void *small_b = kmalloc(8);
    void *small_c = kmalloc(16);

    if (small_a == NULL ||
        small_b == NULL ||
        small_c == NULL) {

        kfree(small_a);
        kfree(small_b);
        kfree(small_c);

        return heap_test_failure(
            "Small slab allocations"
        );
    }

    heap_test_success(
        "Small slab allocations"
    );


    /*
     * TEST 3:
     * Small objects must be 16-byte aligned.
     */
    if (((uintptr_t)small_a & 15ULL) != 0 ||
        ((uintptr_t)small_b & 15ULL) != 0 ||
        ((uintptr_t)small_c & 15ULL) != 0) {

        kfree(small_a);
        kfree(small_b);
        kfree(small_c);

        return heap_test_failure(
            "Slab object alignment"
        );
    }

    heap_test_success(
        "Slab object alignment"
    );


    /*
     * TEST 4:
     * Objects of the same class should share one slab.
     */
    if (((uintptr_t)small_a >> 12) !=
            ((uintptr_t)small_b >> 12) ||

        ((uintptr_t)small_a >> 12) !=
            ((uintptr_t)small_c >> 12)) {

        kfree(small_a);
        kfree(small_b);
        kfree(small_c);

        return heap_test_failure(
            "Same-class slab sharing"
        );
    }

    heap_test_success(
        "Same-class slab sharing"
    );


    /*
     * TEST 5:
     * Objects must have distinct addresses.
     */
    if (small_a == small_b ||
        small_a == small_c ||
        small_b == small_c) {

        kfree(small_a);
        kfree(small_b);
        kfree(small_c);

        return heap_test_failure(
            "Slab object address uniqueness"
        );
    }

    heap_test_success(
        "Slab object address uniqueness"
    );


    /*
     * TEST 6:
     * Allocated objects must be writable.
     */
    volatile uint8_t *a =
        (volatile uint8_t *)small_a;

    volatile uint8_t *b =
        (volatile uint8_t *)small_b;

    volatile uint8_t *c =
        (volatile uint8_t *)small_c;

    a[0] = 0xA5;
    b[0] = 0x5A;
    c[0] = 0xC3;

    if (a[0] != 0xA5 ||
        b[0] != 0x5A ||
        c[0] != 0xC3) {

        kfree(small_a);
        kfree(small_b);
        kfree(small_c);

        return heap_test_failure(
            "Slab object read and write"
        );
    }

    heap_test_success(
        "Slab object read and write"
    );


    /*
     * TEST 7:
     * Interior pointers must not free slab objects.
     */
    uint64_t frames_before_invalid_free =
        pmm_get_free_frames();

    kfree((uint8_t *)small_a + 1);

    if (pmm_get_free_frames() !=
        frames_before_invalid_free) {

        kfree(small_a);
        kfree(small_b);
        kfree(small_c);

        return heap_test_failure(
            "Slab interior-pointer rejection"
        );
    }

    heap_test_success(
        "Slab interior-pointer rejection"
    );


    /*
     * TEST 8:
     * Double-free must not decrement slab allocation count.
     */
    kfree(small_a);
    kfree(small_a);

    kfree(small_b);
    kfree(small_c);

    if (pmm_get_free_frames() != frames_before) {

        return heap_test_failure(
            "Slab free and double-free accounting"
        );
    }

    heap_test_success(
        "Slab free and double-free accounting"
    );


    /*
     * TEST 9:
     * Verify multiple size classes.
     */
    void *class_32 = kmalloc(17);
    void *class_64 = kmalloc(64);
    void *class_128 = kmalloc(100);
    void *class_256 = kmalloc(200);
    void *class_512 = kmalloc(500);
    void *class_1024 = kmalloc(1024);

    if (class_32 == NULL ||
        class_64 == NULL ||
        class_128 == NULL ||
        class_256 == NULL ||
        class_512 == NULL ||
        class_1024 == NULL) {

        kfree(class_32);
        kfree(class_64);
        kfree(class_128);
        kfree(class_256);
        kfree(class_512);
        kfree(class_1024);

        return heap_test_failure(
            "All slab size classes"
        );
    }

    heap_test_success(
        "All slab size classes"
    );


    /*
     * TEST 10:
     * Different size classes must have separate slab pages.
     */
    if (((uintptr_t)class_32 >> 12) ==
            ((uintptr_t)class_64 >> 12) ||

        ((uintptr_t)class_64 >> 12) ==
            ((uintptr_t)class_128 >> 12)) {

        kfree(class_32);
        kfree(class_64);
        kfree(class_128);
        kfree(class_256);
        kfree(class_512);
        kfree(class_1024);

        return heap_test_failure(
            "Separate slab pages for size classes"
        );
    }

    heap_test_success(
        "Separate slab pages for size classes"
    );


    kfree(class_32);
    kfree(class_64);
    kfree(class_128);
    kfree(class_256);
    kfree(class_512);
    kfree(class_1024);

    if (pmm_get_free_frames() != frames_before) {

        return heap_test_failure(
            "Size-class slab reclamation"
        );
    }

    heap_test_success(
        "Size-class slab reclamation"
    );


    /*
     * TEST 11:
     * Fill a 16-byte slab beyond its capacity.
     *
     * A single 4 KiB page holds 256 objects of 16 bytes.
     * The 257th allocation must create a second slab.
     */
    void *objects[HEAP_TEST_SLAB_OBJECTS] = {0};

    bool mass_allocation_success = true;

    for (uint64_t i = 0;
         i < HEAP_TEST_SLAB_OBJECTS;
         i++) {

        objects[i] = kmalloc(16);

        if (objects[i] == NULL) {
            mass_allocation_success = false;
            break;
        }
    }

    if (!mass_allocation_success) {

        for (uint64_t i = 0;
             i < HEAP_TEST_SLAB_OBJECTS;
             i++) {

            kfree(objects[i]);
        }

        return heap_test_failure(
            "Slab capacity expansion"
        );
    }

    /*
     * The first 256 objects occupy one slab.
     * The 257th must be backed by another page.
     */
    if (((uintptr_t)objects[0] >> 12) ==
        ((uintptr_t)objects[256] >> 12)) {

        for (uint64_t i = 0;
             i < HEAP_TEST_SLAB_OBJECTS;
             i++) {

            kfree(objects[i]);
        }

        return heap_test_failure(
            "Slab expansion beyond object capacity"
        );
    }

    heap_test_success(
        "Slab expansion beyond object capacity"
    );


    /*
     * All 257 objects must have distinct addresses.
     */
    for (uint64_t i = 0;
         i < HEAP_TEST_SLAB_OBJECTS;
         i++) {

        for (uint64_t j = i + 1;
             j < HEAP_TEST_SLAB_OBJECTS;
             j++) {

            if (objects[i] == objects[j]) {

                for (uint64_t k = 0;
                     k < HEAP_TEST_SLAB_OBJECTS;
                     k++) {

                    kfree(objects[k]);
                }

                return heap_test_failure(
                    "Mass slab allocation uniqueness"
                );
            }
        }
    }

    heap_test_success(
        "Mass slab allocation uniqueness"
    );


    /*
     * Release all 257 objects.
     * Both empty slabs should return their backing pages.
     */
    for (uint64_t i = 0;
         i < HEAP_TEST_SLAB_OBJECTS;
         i++) {

        kfree(objects[i]);
    }

    if (pmm_get_free_frames() != frames_before) {

        return heap_test_failure(
            "Mass slab reclamation and PMM accounting"
        );
    }

    heap_test_success(
        "Mass slab reclamation and PMM accounting"
    );


    /*
     * TEST 12:
     * Large allocations must still use the page allocator.
     */
    void *large = kmalloc(HEAP_TEST_PAGE_SIZE + 1);

    if (large == NULL) {

        return heap_test_failure(
            "Large page-backed allocation"
        );
    }

    if (((uintptr_t)large &
            (HEAP_TEST_PAGE_SIZE - 1)) != 0) {

        kfree(large);

        return heap_test_failure(
            "Large allocation page alignment"
        );
    }

    volatile uint8_t *large_memory =
        (volatile uint8_t *)large;

    large_memory[0] = 0x12;
    large_memory[HEAP_TEST_PAGE_SIZE] = 0x34;

    if (large_memory[0] != 0x12 ||
        large_memory[HEAP_TEST_PAGE_SIZE] != 0x34) {

        kfree(large);

        return heap_test_failure(
            "Large allocation read and write"
        );
    }

    heap_test_success(
        "Large page-backed allocation"
    );

    kfree(large);

    if (pmm_get_free_frames() != frames_before) {

        return heap_test_failure(
            "Large allocation cleanup"
        );
    }

    heap_test_success(
        "Large allocation cleanup"
    );


    /*
     * TEST 13:
     * Oversized allocation must fail safely.
     */
    if (kmalloc((size_t)-1) != NULL) {

        return heap_test_failure(
            "Oversized allocation rejection"
        );
    }

    heap_test_success(
        "Oversized allocation rejection"
    );

    /*
     * TEST 14:
     * Named object-cache creation and duplicate-name rejection.
     */
    uint64_t cache_frames_before =
        pmm_get_free_frames();

    heap_test_constructor_calls = 0;
    heap_test_destructor_calls = 0;

    heap_cache_t *object_cache = heap_cache_create(
        "test_object_cache",
        sizeof(heap_test_object_t),
        heap_test_object_constructor,
        heap_test_object_destructor
    );

    if (object_cache == NULL) {
        return heap_test_failure(
            "Named object-cache creation"
        );
    }

    if (heap_cache_create(
            "test_object_cache",
            sizeof(heap_test_object_t),
            NULL,
            NULL) != NULL) {

        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Duplicate cache-name rejection"
        );
    }

    heap_test_success(
        "Named object-cache creation and duplicate-name rejection"
    );


    /*
     * TEST 15:
     * Allocate objects and verify constructor behavior.
     */
    heap_test_object_t *object_a =
        heap_cache_alloc(object_cache);

    heap_test_object_t *object_b =
        heap_cache_alloc(object_cache);

    if (object_a == NULL || object_b == NULL) {
        if (object_a != NULL) {
            heap_cache_free(object_cache, object_a);
        }

        if (object_b != NULL) {
            heap_cache_free(object_cache, object_b);
        }

        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Named cache object allocation"
        );
    }

    if (heap_test_constructor_calls != 2 ||
        object_a->magic != HEAP_TEST_OBJECT_MAGIC ||
        object_b->magic != HEAP_TEST_OBJECT_MAGIC ||
        object_a == object_b) {

        heap_cache_free(object_cache, object_a);
        heap_cache_free(object_cache, object_b);
        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Named cache constructors and object uniqueness"
        );
    }

    object_a->value = 100;
    object_b->value = 200;

    heap_test_success(
        "Named cache allocation and constructors"
    );


    /*
     * TEST 16:
     * Verify cache statistics.
     */
    heap_cache_stats_t cache_stats;

    if (!heap_cache_get_stats(
            object_cache,
            &cache_stats) ||
        cache_stats.object_size != 16 ||
        cache_stats.active_objects != 2 ||
        cache_stats.total_allocations != 2 ||
        cache_stats.total_frees != 0 ||
        cache_stats.slab_count != 1) {

        heap_cache_free(object_cache, object_a);
        heap_cache_free(object_cache, object_b);
        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Named cache allocation statistics"
        );
    }

    heap_test_success(
        "Named cache allocation statistics"
    );


    /*
     * TEST 17:
     * Reject freeing an object through the wrong cache.
     */
    heap_cache_t *other_cache = heap_cache_create(
        "other_test_cache",
        sizeof(heap_test_object_t),
        NULL,
        NULL
    );

    if (other_cache == NULL) {
        heap_cache_free(object_cache, object_a);
        heap_cache_free(object_cache, object_b);
        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Second named cache creation"
        );
    }

    if (heap_cache_free(other_cache, object_a)) {
        return heap_test_failure(
            "Cross-cache ownership rejection"
        );
    }

    /*
     * Ordinary kfree must not release named-cache objects.
     */
    uint64_t frames_before_wrong_free =
        pmm_get_free_frames();

    kfree(object_a);

    if (pmm_get_free_frames() != frames_before_wrong_free ||
        object_a->magic != HEAP_TEST_OBJECT_MAGIC) {

        heap_cache_free(object_cache, object_a);
        heap_cache_free(object_cache, object_b);
        heap_cache_destroy(other_cache);
        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Ordinary free rejects named-cache objects"
        );
    }

    heap_test_success(
        "Named-cache ownership enforcement"
    );


    /*
     * TEST 18:
     * A cache cannot be destroyed while objects are allocated.
     */
    if (heap_cache_destroy(object_cache)) {
        return heap_test_failure(
            "Active cache destruction rejection"
        );
    }

    heap_test_success(
        "Active cache destruction rejection"
    );


    /*
     * TEST 19:
     * Correct freeing, destructor execution and double-free rejection.
     */
    if (!heap_cache_free(object_cache, object_a)) {
        return heap_test_failure(
            "Named cache object release"
        );
    }

    if (heap_test_destructor_calls != 1 ||
        object_a->magic != 0) {

        heap_cache_free(object_cache, object_b);
        heap_cache_destroy(other_cache);
        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Named cache destructor"
        );
    }

    if (heap_cache_free(object_cache, object_a)) {
        return heap_test_failure(
            "Named cache double-free rejection"
        );
    }

    if (!heap_cache_free(object_cache, object_b)) {
        return heap_test_failure(
            "Second named cache object release"
        );
    }

    if (heap_test_destructor_calls != 2) {
        return heap_test_failure(
            "Named cache destructor accounting"
        );
    }

    heap_test_success(
        "Named cache release, destructor and double-free protection"
    );


    /*
     * TEST 20:
     * Verify final statistics and destroy empty caches.
     */
    if (!heap_cache_get_stats(
            object_cache,
            &cache_stats) ||
        cache_stats.active_objects != 0 ||
        cache_stats.total_allocations != 2 ||
        cache_stats.total_frees != 2 ||
        cache_stats.slab_count != 0) {

        heap_cache_destroy(other_cache);
        heap_cache_destroy(object_cache);

        return heap_test_failure(
            "Named cache final statistics"
        );
    }

    if (!heap_cache_destroy(other_cache) ||
        !heap_cache_destroy(object_cache)) {

        return heap_test_failure(
            "Empty named cache destruction"
        );
    }

    if (pmm_get_free_frames() != cache_frames_before) {
        return heap_test_failure(
            "Named cache PMM reclamation"
        );
    }

    heap_test_success(
        "Named cache destruction and PMM reclamation"
    );

    /*
     * TEST 21:
     * kcalloc must return zero-initialized memory.
     */
    uint8_t *zero_array = (uint8_t *)kcalloc(4, 8);

    if (zero_array == NULL) {
        return heap_test_failure(
            "kcalloc allocation"
        );
    }

    bool zero_array_valid = true;

    for (uint64_t i = 0; i < 32; i++) {
        if (zero_array[i] != 0) {
            zero_array_valid = false;
            break;
        }
    }

    kfree(zero_array);

    if (!zero_array_valid) {
        return heap_test_failure(
            "kcalloc zero initialization"
        );
    }

    heap_test_success(
        "kcalloc zero initialization"
    );


    /*
     * TEST 22:
     * kcalloc must reject multiplication overflow.
     */
    if (kcalloc(SIZE_MAX, 2) != NULL) {
        return heap_test_failure(
            "kcalloc multiplication overflow protection"
        );
    }

    heap_test_success(
        "kcalloc multiplication overflow protection"
    );


    /*
     * TEST 23:
     * krealloc(NULL, size) must behave like kmalloc().
     */
    void *realloc_null = krealloc(NULL, 32);

    if (realloc_null == NULL) {
        return heap_test_failure(
            "krealloc NULL pointer allocation"
        );
    }

    kfree(realloc_null);

    heap_test_success(
        "krealloc NULL pointer allocation"
    );


    /*
     * TEST 24:
     * Growing within the existing slab capacity should
     * preserve the pointer and existing contents.
     */
    uint8_t *realloc_data =
        (uint8_t *)kmalloc(24);

    if (realloc_data == NULL) {
        return heap_test_failure(
            "krealloc initial slab allocation"
        );
    }

    for (uint64_t i = 0; i < 24; i++) {
        realloc_data[i] = (uint8_t)(i + 1);
    }

    uint8_t *same_pointer =
        (uint8_t *)krealloc(realloc_data, 28);

    if (same_pointer != realloc_data) {
        kfree(same_pointer);
        kfree(realloc_data);

        return heap_test_failure(
            "krealloc in-place slab resize"
        );
    }

    for (uint64_t i = 0; i < 24; i++) {
        if (same_pointer[i] != (uint8_t)(i + 1)) {
            kfree(same_pointer);

            return heap_test_failure(
                "krealloc in-place data preservation"
            );
        }
    }

    realloc_data = same_pointer;

    heap_test_success(
        "krealloc in-place slab resize and data preservation"
    );


    /*
     * TEST 25:
     * Grow from a slab allocation to a page-backed allocation.
     */
    uint8_t *large_data =
        (uint8_t *)krealloc(realloc_data, 2048);

    if (large_data == NULL ||
        large_data == realloc_data) {

        kfree(large_data);
        kfree(realloc_data);

        return heap_test_failure(
            "krealloc slab-to-page growth"
        );
    }

    for (uint64_t i = 0; i < 24; i++) {
        if (large_data[i] != (uint8_t)(i + 1)) {
            kfree(large_data);

            return heap_test_failure(
                "krealloc slab-to-page data preservation"
            );
        }
    }

    heap_test_success(
        "krealloc slab-to-page growth and data preservation"
    );


    /*
     * TEST 26:
     * Grow an allocation beyond its existing page capacity.
     */
    uint8_t *larger_data =
        (uint8_t *)krealloc(large_data, 8192);

    if (larger_data == NULL ||
        larger_data == large_data) {

        kfree(larger_data);
        kfree(large_data);

        return heap_test_failure(
            "krealloc page-backed growth"
        );
    }

    for (uint64_t i = 0; i < 24; i++) {
        if (larger_data[i] != (uint8_t)(i + 1)) {
            kfree(larger_data);

            return heap_test_failure(
                "krealloc page-backed data preservation"
            );
        }
    }

    heap_test_success(
        "krealloc page-backed growth and data preservation"
    );


    /*
     * TEST 27:
     * Failed growth must leave the original allocation valid.
     */
    void *failed_growth = krealloc(
        larger_data,
        32ULL * 1024ULL * 1024ULL
    );

    if (failed_growth != NULL) {
        kfree(failed_growth);
        kfree(larger_data);

        return heap_test_failure(
            "krealloc allocation failure handling"
        );
    }

    for (uint64_t i = 0; i < 24; i++) {
        if (larger_data[i] != (uint8_t)(i + 1)) {
            kfree(larger_data);

            return heap_test_failure(
                "krealloc original allocation preservation after failure"
            );
        }
    }

    heap_test_success(
        "krealloc preserves original allocation after failure"
    );


    /*
     * TEST 28:
     * Shrinking within existing page capacity should work.
     */
    uint8_t *shrunk_data =
        (uint8_t *)krealloc(larger_data, 64);

    if (shrunk_data != larger_data) {
        kfree(shrunk_data);
        kfree(larger_data);

        return heap_test_failure(
            "krealloc in-place page-backed shrink"
        );
    }

    for (uint64_t i = 0; i < 24; i++) {
        if (shrunk_data[i] != (uint8_t)(i + 1)) {
            kfree(shrunk_data);

            return heap_test_failure(
                "krealloc shrink data preservation"
            );
        }
    }

    heap_test_success(
        "krealloc in-place page-backed shrink"
    );


    /*
     * TEST 29:
     * Invalid pointers must be rejected.
     */
    if (krealloc((void *)(uintptr_t)0x1234, 32) != NULL) {
        kfree(shrunk_data);

        return heap_test_failure(
            "krealloc invalid pointer rejection"
        );
    }

    heap_test_success(
        "krealloc invalid pointer rejection"
    );


    /*
     * TEST 30:
     * A zero-sized resize frees the allocation.
     */
    if (krealloc(shrunk_data, 0) != NULL) {
        return heap_test_failure(
            "krealloc zero-size release"
        );
    }

    heap_test_success(
        "krealloc zero-size release"
    );


    heap_test_success(
        "Kernel heap slab allocator tests"
    );

    console_write("===== HEAP TESTS COMPLETE =====\n\n");

    return true;
}
