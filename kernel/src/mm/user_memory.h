#ifndef HUA_USER_MEMORY_H
#define HUA_USER_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm/vmm.h"
#include "mm/pmm.h"

/*
 * Access requested for a user-memory range.
 *
 * User-readable memory is represented by a present,
 * user-accessible mapping.
 *
 * Writable memory must additionally have the effective
 * writable permission.
 */
typedef enum {

    USER_MEMORY_ACCESS_NONE  = 0,

    USER_MEMORY_ACCESS_READ  = (1U << 0),

    USER_MEMORY_ACCESS_WRITE = (1U << 1)

} user_memory_access_t;


/*
 * User-memory operation results.
 */
typedef enum {

    USER_MEMORY_STATUS_OK = 0,

    USER_MEMORY_STATUS_INVALID_ARGUMENT,

    USER_MEMORY_STATUS_RANGE_OVERFLOW,

    USER_MEMORY_STATUS_NOT_USER_RANGE,

    USER_MEMORY_STATUS_TOO_LARGE,

    USER_MEMORY_STATUS_NOT_MAPPED,

    USER_MEMORY_STATUS_NOT_USER_ACCESSIBLE,

    USER_MEMORY_STATUS_NOT_WRITABLE,

    USER_MEMORY_STATUS_UNSUPPORTED_BACKING,

    USER_MEMORY_STATUS_ADDRESS_SPACE_ERROR,

    USER_MEMORY_STATUS_PREEMPTION_ERROR,

    USER_MEMORY_STATUS_INTERNAL_ERROR

} user_memory_status_t;


/*
 * Bound the amount of memory copied in one operation.
 *
 * This limits:
 * - Time spent with preemption disabled.
 * - Temporary page-alias storage.
 * - Work performed on an untrusted syscall request.
 *
 * Larger transfers should eventually be divided into
 * multiple bounded operations.
 */
#define USER_MEMORY_MAX_COPY_BYTES (64ULL * 1024ULL)


/*
 * Maximum number of 4 KiB pages covered by a 64 KiB
 * range, including the possibility of an unaligned start.
 */
#define USER_MEMORY_MAX_PAGES \
    (((USER_MEMORY_MAX_COPY_BYTES + PMM_PAGE_SIZE - 1ULL) / \
      PMM_PAGE_SIZE) + 1ULL)


/*
 * Validate a user virtual address range.
 *
 * space:
 *     The address space belonging to the calling process.
 *
 * user_address:
 *     First user virtual address.
 *
 * length:
 *     Number of bytes in the requested range.
 *
 * access:
 *     USER_MEMORY_ACCESS_READ,
 *     USER_MEMORY_ACCESS_WRITE,
 *     or both.
 *
 * A zero-length range is a successful no-op. Its address
 * is not dereferenced or required to be mapped.
 */
user_memory_status_t user_range_validate(
    vmm_address_space_t *space,
    uint64_t user_address,
    size_t length,
    user_memory_access_t access
);


/*
 * Copy data from a user process into a kernel buffer.
 *
 * kernel_destination must be a valid kernel-owned buffer.
 * The user source must be readable for the complete range.
 *
 * The operation never dereferences user_address directly.
 *
 * Validation failures occur before the destination is changed.
 */
user_memory_status_t copy_from_user(
    vmm_address_space_t *space,
    void *kernel_destination,
    uint64_t user_source,
    size_t length
);


/*
 * Copy data from a kernel buffer into a user process.
 *
 * kernel_source must be a valid kernel-owned buffer.
 * The user destination must be writable for the complete range.
 *
 * The operation never dereferences user_address directly.
 *
 * Validation failures occur before the user destination is changed.
 */
user_memory_status_t copy_to_user(
    vmm_address_space_t *space,
    uint64_t user_destination,
    const void *kernel_source,
    size_t length
);

#endif
