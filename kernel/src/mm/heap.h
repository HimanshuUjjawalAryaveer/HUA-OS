
#ifndef HEAP_H
#define HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Opaque handle for a named kernel object cache.
 */
typedef struct heap_cache heap_cache_t;

/*
 * A constructor returns true when initialization succeeds.
 * A destructor runs immediately before an allocated object
 * is returned to its cache.
 */
typedef bool (*heap_cache_constructor_t)(void *object);
typedef void (*heap_cache_destructor_t)(void *object);

/*
 * Statistics for one named cache.
 */
typedef struct {
    size_t object_size;
    uint64_t active_objects;
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t slab_count;
} heap_cache_stats_t;

bool heap_init(void);

void *kmalloc(size_t size);
void kfree(void *ptr);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t new_size);

/*
 * Named fixed-size object caches.
 *
 * Supported requested object sizes: 1 through 1024 bytes.
 * Internally, object sizes are rounded up to 16-byte alignment.
 */
heap_cache_t *heap_cache_create(
    const char *name,
    size_t object_size,
    heap_cache_constructor_t constructor,
    heap_cache_destructor_t destructor
);

void *heap_cache_alloc(heap_cache_t *cache);
bool heap_cache_free(heap_cache_t *cache, void *ptr);
bool heap_cache_destroy(heap_cache_t *cache);

bool heap_cache_get_stats(
    const heap_cache_t *cache,
    heap_cache_stats_t *stats
);

#endif
