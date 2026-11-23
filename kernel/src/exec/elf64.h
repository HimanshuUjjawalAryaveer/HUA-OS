#ifndef HUA_EXEC_ELF64_H
#define HUA_EXEC_ELF64_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ELF64 loader policy.
 *
 * These limits deliberately constrain the first executable
 * format supported by HUA.
 */
#define ELF64_PAGE_SIZE                 4096ULL
#define ELF64_PAGE_MASK                 (ELF64_PAGE_SIZE - 1ULL)

#define ELF64_USER_MIN_ADDRESS          0x0000000000010000ULL
#define ELF64_USER_ADDRESS_LIMIT        0x0000800000000000ULL

#define ELF64_MAX_PROGRAM_HEADERS       128U
#define ELF64_MAX_LOAD_SEGMENTS         32U
#define ELF64_MAX_IMAGE_PAGES           16384ULL

/*
 * ELF64 program-header permission bits.
 */
#define ELF64_PF_X                      0x1U
#define ELF64_PF_W                      0x2U
#define ELF64_PF_R                      0x4U

/*
 * Parser result.
 */
typedef enum {

    ELF64_STATUS_OK = 0,

    ELF64_STATUS_INVALID_ARGUMENT,

    ELF64_STATUS_TRUNCATED,

    ELF64_STATUS_INVALID_FORMAT,

    ELF64_STATUS_UNSUPPORTED,

    ELF64_STATUS_LIMIT_EXCEEDED,

    ELF64_STATUS_INVALID_SEGMENT,

    ELF64_STATUS_PERMISSION_CONFLICT,

    ELF64_STATUS_INVALID_ENTRY,

    ELF64_STATUS_INSUFFICIENT_CAPACITY,

    ELF64_STATUS_OUT_OF_MEMORY,

    ELF64_STATUS_MEMORY_ACCESS_FAILED,

    ELF64_STATUS_CLEANUP_FAILED

} elf64_status_t;

/*
 * One validated PT_LOAD segment.
 *
 * memory_end and page_end are exclusive addresses.
 *
 * file_data is not copied or owned by this structure.
 * It remains a borrowed pointer to the original ELF image.
 */
typedef struct {

    uint64_t file_offset;

    uint64_t virtual_address;

    uint64_t file_size;

    uint64_t memory_size;

    uint64_t memory_end;

    uint64_t page_start;

    uint64_t page_end;

    uint64_t alignment;

    uint32_t flags;

} elf64_load_segment_t;

/*
 * Validated executable description.
 *
 * This is a parsed description, not a loaded process.
 *
 * The caller must keep file_data valid while using this
 * structure to load the executable.
 */
typedef struct {

    const uint8_t *file_data;

    uint64_t file_size;

    uint64_t entry;

    uint16_t program_header_count;

    uint16_t load_segment_count;

    elf64_load_segment_t segments[
        ELF64_MAX_LOAD_SEGMENTS
    ];

} elf64_image_t;

/*
 * One unique 4 KiB page required by a parsed ELF image.
 *
 * Entries produced by elf64_build_page_plan() are sorted
 * by virtual_address and contain no duplicate pages.
 *
 * segment_flags uses ELF64_PF_R, ELF64_PF_W, and ELF64_PF_X.
 *
 * reserved is always zero.
 */
typedef struct {

    uint64_t virtual_address;

    uint32_t segment_flags;

    uint32_t reserved;

} elf64_page_plan_entry_t;

/*
 * One allocated and initialized physical page.
 *
 * The physical frame is owned by the caller until it is
 * explicitly released or transferred to another subsystem.
 *
 * The entries produced by elf64_prepare_pages() are initially
 * sorted by virtual_address.
 */
typedef struct {

    uint64_t virtual_address;

    uint64_t physical_address;

    uint32_t segment_flags;

    uint32_t reserved;

} elf64_prepared_page_t;

/*
 * Parse and validate an ELF64 executable.
 *
 * Success:
 *     ELF64_STATUS_OK
 *     *image_out receives the complete validated image.
 *
 * Failure:
 *     Returns a specific error.
 *     *image_out remains unchanged.
 *
 * The parser:
 *     - Does not allocate memory.
 *     - Does not modify the supplied ELF bytes.
 *     - Does not create page mappings.
 *     - Does not depend on Limine or the process manager.
 */
elf64_status_t elf64_parse(
    const void *file_data,
    uint64_t file_size,
    elf64_image_t *image_out
);

/*
 * Build a sorted, deduplicated list of the 4 KiB pages
 * needed by a validated ELF image.
 *
 * The caller supplies the output array. This avoids a
 * large fixed-size plan on the kernel stack and makes
 * the planner independent of the kernel heap.
 *
 * Success:
 *     Returns ELF64_STATUS_OK.
 *     *page_count_out receives the number of unique pages.
 *
 * Failure:
 *     *page_count_out remains unchanged.
 *     The output array remains unchanged.
 *
 * The image must remain valid during this call.
 * The output array must not overlap the image or its
 * underlying file data.
 *
 * This function does not allocate physical memory or
 * change any page tables.
 */
elf64_status_t elf64_build_page_plan(
    const elf64_image_t *image,
    elf64_page_plan_entry_t *page_storage,
    uint64_t page_capacity,
    uint64_t *page_count_out
);

/*
 * Allocate and populate all physical pages required by an
 * ELF image.
 *
 * The image and its file_data must remain valid and unchanged
 * for the duration of this call.
 *
 * The caller supplies storage for the prepared-page records.
 *
 * Success:
 *     Returns ELF64_STATUS_OK.
 *     *page_count_out receives the number of allocated pages.
 *     The caller owns every physical frame in the output.
 *
 * Normal failure:
 *     All frames allocated by this operation are released.
 *     *page_count_out remains unchanged.
 *
 * Cleanup failure:
 *     Returns ELF64_STATUS_CLEANUP_FAILED.
 *     *page_count_out receives the number of frames that could
 *     not be released. Their records remain in the first entries
 *     of page_storage. The caller must release those records.
 *
 * No page-table mappings are created.
 */
elf64_status_t elf64_prepare_pages(
    const elf64_image_t *image,
    elf64_prepared_page_t *page_storage,
    uint64_t page_capacity,
    uint64_t *page_count_out
);


/*
 * Release physical frames recorded by elf64_prepare_pages().
 *
 * page_count_io is both the input number of records and the
 * output number of records that remain unreleased.
 *
 * Success:
 *     All frames were released and *page_count_io is zero.
 *
 * Cleanup failure:
 *     Unreleased records are retained at the beginning of
 *     page_storage, and *page_count_io reports their count.
 *
 * This operation may reorder page_storage.
 */
elf64_status_t elf64_release_prepared_pages(
    elf64_prepared_page_t *page_storage,
    uint64_t *page_count_io
);

/*
 * Return a human-readable description of a parser result.
 */
const char *elf64_status_string(
    elf64_status_t status
);

#endif
