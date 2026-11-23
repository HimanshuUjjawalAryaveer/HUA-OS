#include "exec/elf64.h"
#include <stddef.h>
#include "lib/memory.h"
#include "mm/pmm.h"

/*
 * ELF identification indexes.
 */
#define ELF_EI_CLASS       4U
#define ELF_EI_DATA        5U
#define ELF_EI_VERSION     6U
#define ELF_EI_OSABI       7U
#define ELF_EI_ABIVERSION  8U
#define ELF_EI_PAD_START   9U

/*
 * ELF identification values.
 */
#define ELF_CLASS_64       2U
#define ELF_DATA_LSB       1U
#define ELF_VERSION_1      1U
#define ELF_OSABI_SYSV     0U

/*
 * ELF object types and architecture.
 */
#define ELF_ET_EXEC        2U
#define ELF_EM_X86_64      62U

/*
 * ELF structure sizes.
 */
#define ELF_HEADER_SIZE           64U
#define ELF_PROGRAM_HEADER_SIZE   56U

/*
 * ELF program-header types.
 */
#define ELF_PT_NULL          0U
#define ELF_PT_LOAD          1U
#define ELF_PT_DYNAMIC       2U
#define ELF_PT_INTERP        3U
#define ELF_PT_SHLIB         5U
#define ELF_PT_TLS           7U

#define ELF_PT_GNU_STACK     0x6474E551U
#define ELF_PT_GNU_RELRO     0x6474E552U

/*
 * ELF header offsets.
 */
#define ELF_E_TYPE           16U
#define ELF_E_MACHINE        18U
#define ELF_E_VERSION        20U
#define ELF_E_ENTRY          24U
#define ELF_E_PHOFF          32U
#define ELF_E_FLAGS          48U
#define ELF_E_EHSIZE         52U
#define ELF_E_PHENTSIZE      54U
#define ELF_E_PHNUM          56U

/*
 * ELF64 program-header offsets.
 */
#define ELF_PH_TYPE          0U
#define ELF_PH_FLAGS         4U
#define ELF_PH_OFFSET        8U
#define ELF_PH_VADDR         16U
#define ELF_PH_FILESZ        32U
#define ELF_PH_MEMSZ         40U
#define ELF_PH_ALIGN         48U

/*
 * A valid ELF file starts with these four bytes.
 */
static const uint8_t elf_magic[4] = {
    0x7F,
    'E',
    'L',
    'F'
};

/*
 * Check whether two pointer ranges overlap.
 *
 * Defined later in this file. This declaration allows
 * page-plan construction to use it before its definition.
 */
static bool elf_ranges_overlap(
    const void *first,
    uint64_t first_size,
    const void *second,
    uint64_t second_size
);

/*
 * Read an unsigned 16-bit little-endian integer.
 *
 * Byte-wise reading avoids:
 *     - Unaligned structure accesses.
 *     - Compiler-dependent structure packing.
 *     - Strict-aliasing problems.
 */
static uint16_t elf_read_u16(
    const uint8_t *bytes
) {
    return
        (uint16_t)bytes[0] |
        ((uint16_t)bytes[1] << 8);
}


/*
 * Read an unsigned 32-bit little-endian integer.
 */
static uint32_t elf_read_u32(
    const uint8_t *bytes
) {
    return
        (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}


/*
 * Read an unsigned 64-bit little-endian integer.
 */
static uint64_t elf_read_u64(
    const uint8_t *bytes
) {
    uint64_t value = 0;

    for (uint32_t i = 0; i < 8; i++) {

        value |=
            ((uint64_t)bytes[i]) << (i * 8U);
    }

    return value;
}


/*
 * Verify that an integer is a power of two.
 */
static bool elf_is_power_of_two(
    uint64_t value
) {
    return value != 0 &&
        (value & (value - 1ULL)) == 0;
}


/*
 * Verify that an address range is inside HUA's initial
 * supported user-address interval.
 *
 * end is exclusive.
 */
static bool elf_validate_user_range(
    uint64_t start,
    uint64_t end
) {
    if (start < ELF64_USER_MIN_ADDRESS ||
        start >= ELF64_USER_ADDRESS_LIMIT ||
        end <= start ||
        end > ELF64_USER_ADDRESS_LIMIT) {

        return false;
    }

    return true;
}


/*
 * Validate the common properties of one PT_LOAD segment.
 *
 * The returned segment description is fully checked and
 * safe for later page-plan construction.
 */
static elf64_status_t elf_validate_load_segment(
    const uint8_t *file_data,
    uint64_t file_size,
    uint64_t file_offset,
    uint64_t virtual_address,
    uint64_t file_bytes,
    uint64_t memory_bytes,
    uint64_t alignment,
    uint32_t flags,
    elf64_load_segment_t *segment_out
) {
    if (file_data == NULL ||
        segment_out == NULL) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Reject permission bits we do not understand.
     */
    if (flags & ~(ELF64_PF_R |
                  ELF64_PF_W |
                  ELF64_PF_X)) {

        return ELF64_STATUS_UNSUPPORTED;
    }

    /*
     * Standard x86-64 paging cannot enforce a general
     * user-readable=false permission.
     *
     * Require PF_R rather than silently granting read access.
     */
    if (!(flags & ELF64_PF_R)) {
        return ELF64_STATUS_UNSUPPORTED;
    }

    /*
     * Reject writable and executable segments.
     */
    if ((flags & ELF64_PF_W) &&
        (flags & ELF64_PF_X)) {

        return ELF64_STATUS_PERMISSION_CONFLICT;
    }

    /*
     * Filesz may never exceed Memsz.
     */
    if (file_bytes > memory_bytes) {
        return ELF64_STATUS_INVALID_SEGMENT;
    }

    /*
     * Validate the file-backed byte range without computing
     * file_offset + file_bytes.
     *
     * This avoids integer overflow.
     */
    if (file_offset > file_size ||
        file_bytes > file_size - file_offset) {

        return ELF64_STATUS_TRUNCATED;
    }

    /*
     * ELF permits p_align values of zero and one.
     *
     * Larger alignment values must be powers of two, and
     * p_vaddr and p_offset must be congruent modulo p_align.
     */
    if (alignment > 1ULL) {

        if (!elf_is_power_of_two(alignment)) {
            return ELF64_STATUS_INVALID_SEGMENT;
        }

        if ((virtual_address & (alignment - 1ULL)) !=
            (file_offset & (alignment - 1ULL))) {

            return ELF64_STATUS_INVALID_SEGMENT;
        }
    }

    /*
     * A zero-sized PT_LOAD contributes no mapped memory.
     *
     * It must not claim file-backed bytes.
     */
    if (memory_bytes == 0) {

        if (file_bytes != 0) {
            return ELF64_STATUS_INVALID_SEGMENT;
        }

        segment_out->file_offset = file_offset;
        segment_out->virtual_address = virtual_address;
        segment_out->file_size = 0;
        segment_out->memory_size = 0;
        segment_out->memory_end = virtual_address;
        segment_out->page_start = 0;
        segment_out->page_end = 0;
        segment_out->alignment = alignment;
        segment_out->flags = flags;

        return ELF64_STATUS_OK;
    }

    /*
     * Validate the user virtual-address range using
     * subtraction to avoid overflow.
     */
    if (virtual_address < ELF64_USER_MIN_ADDRESS ||
        virtual_address >= ELF64_USER_ADDRESS_LIMIT ||
        memory_bytes >
            ELF64_USER_ADDRESS_LIMIT - virtual_address) {

        return ELF64_STATUS_INVALID_SEGMENT;
    }

    uint64_t memory_end =
        virtual_address + memory_bytes;

    if (!elf_validate_user_range(
            virtual_address,
            memory_end
        )) {

        return ELF64_STATUS_INVALID_SEGMENT;
    }

    /*
     * Construct the page-aligned mapping range.
     *
     * The segment itself need not begin or end on a page
     * boundary. The actual loader will handle its offsets
     * within the first and last pages.
     */
    uint64_t page_start =
        virtual_address & ~ELF64_PAGE_MASK;

    if (memory_end >
        UINT64_MAX - ELF64_PAGE_MASK) {

        return ELF64_STATUS_INVALID_SEGMENT;
    }

    uint64_t page_end =
        (memory_end + ELF64_PAGE_MASK) &
        ~ELF64_PAGE_MASK;

    if (page_start < ELF64_USER_MIN_ADDRESS ||
        page_end > ELF64_USER_ADDRESS_LIMIT ||
        page_end <= page_start) {

        return ELF64_STATUS_INVALID_SEGMENT;
    }

    /*
     * Commit the validated description.
     */
    segment_out->file_offset = file_offset;
    segment_out->virtual_address = virtual_address;
    segment_out->file_size = file_bytes;
    segment_out->memory_size = memory_bytes;
    segment_out->memory_end = memory_end;
    segment_out->page_start = page_start;
    segment_out->page_end = page_end;
    segment_out->alignment = alignment;
    segment_out->flags = flags;

    return ELF64_STATUS_OK;
}


/*
 * Sort segment indices by page_start.
 *
 * At most ELF64_MAX_LOAD_SEGMENTS entries exist, so
 * insertion sort is sufficient and avoids an allocator.
 *
 * For identical page_start values, larger page_end values
 * come first. This makes nested page ranges easy to merge.
 */
static void elf_sort_segment_indices_by_page_start(
    const elf64_image_t *image,
    uint16_t indices[ELF64_MAX_LOAD_SEGMENTS]
) {
    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        indices[i] = i;
    }

    for (uint16_t i = 1;
         i < image->load_segment_count;
         i++) {

        uint16_t current_index = indices[i];

        const elf64_load_segment_t *current =
            &image->segments[current_index];

        uint16_t position = i;

        while (position > 0) {

            uint16_t previous_index =
                indices[position - 1];

            const elf64_load_segment_t *previous =
                &image->segments[previous_index];

            bool current_before_previous =
                current->page_start < previous->page_start ||
                (
                    current->page_start == previous->page_start &&
                    current->page_end > previous->page_end
                );

            if (!current_before_previous) {
                break;
            }

            indices[position] = previous_index;
            position--;
        }

        indices[position] = current_index;
    }
}


/*
 * Count the union of all segment page ranges.
 *
 * The input indices must already be sorted by page_start.
 *
 * Since every range is page-aligned and each segment
 * describes a contiguous interval, we can count the union
 * without building a temporary page array.
 */
static elf64_status_t elf_count_unique_pages(
    const elf64_image_t *image,
    const uint16_t *sorted_indices,
    uint64_t *page_count_out
) {
    if (image == NULL ||
        sorted_indices == NULL ||
        page_count_out == NULL) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    uint64_t unique_pages = 0;
    uint64_t covered_end = 0;

    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        const elf64_load_segment_t *segment =
            &image->segments[sorted_indices[i]];

        uint64_t first_uncovered =
            segment->page_start;

        if (first_uncovered < covered_end) {
            first_uncovered = covered_end;
        }

        if (first_uncovered < segment->page_end) {

            uint64_t new_pages =
                (segment->page_end - first_uncovered) /
                ELF64_PAGE_SIZE;

            if (new_pages >
                ELF64_MAX_IMAGE_PAGES - unique_pages) {

                return ELF64_STATUS_LIMIT_EXCEEDED;
            }

            unique_pages += new_pages;
        }

        if (segment->page_end > covered_end) {
            covered_end = segment->page_end;
        }
    }

    *page_count_out = unique_pages;

    return ELF64_STATUS_OK;
}


/*
 * Validate relationships between all loaded segments.
 *
 * Two independent problems must be prevented:
 *
 * 1. Overlapping memory byte ranges would cause ambiguous
 *    initialization and file-copy ordering.
 *
 * 2. Sharing a page with different write/execute permissions
 *    would require granting permissions that one segment
 *    did not request.
 *
 * The image-page limit applies to unique mapped pages,
 * not the sum of each segment's page count.
 */
static elf64_status_t elf_validate_segment_relationships(
    const elf64_image_t *image
) {
    if (image == NULL) {
        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate every pair of segments.
     */
    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        const elf64_load_segment_t *a =
            &image->segments[i];

        for (uint16_t j = 0; j < i; j++) {

            const elf64_load_segment_t *b =
                &image->segments[j];

            /*
             * Actual memory bytes must not overlap.
             */
            bool memory_overlap =
                a->virtual_address < b->memory_end &&
                b->virtual_address < a->memory_end;

            if (memory_overlap) {
                return ELF64_STATUS_INVALID_SEGMENT;
            }

            /*
             * Determine whether the segments share a 4 KiB
             * page.
             */
            bool page_overlap =
                a->page_start < b->page_end &&
                b->page_start < a->page_end;

            if (!page_overlap) {
                continue;
            }

            /*
             * PF_R is required by the parser.
             * Compare writable/executable permissions.
             */
            uint32_t a_permissions =
                a->flags & (ELF64_PF_W | ELF64_PF_X);

            uint32_t b_permissions =
                b->flags & (ELF64_PF_W | ELF64_PF_X);

            if (a_permissions != b_permissions) {
                return ELF64_STATUS_PERMISSION_CONFLICT;
            }
        }
    }

    /*
     * Count the union of page ranges.
     */
    uint16_t sorted_indices[
        ELF64_MAX_LOAD_SEGMENTS
    ];

    elf_sort_segment_indices_by_page_start(
        image,
        sorted_indices
    );

    uint64_t unique_pages = 0;

    return elf_count_unique_pages(
        image,
        sorted_indices,
        &unique_pages
    );
}

/*
 * Validate an image description before page-plan construction.
 *
 * elf64_image_t is a public C structure and can be modified
 * by its caller. Do not blindly trust its cached fields.
 */
static elf64_status_t elf_validate_page_plan_image(
    const elf64_image_t *image
) {
    if (image == NULL ||
        image->file_data == NULL) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    if (image->file_size < ELF_HEADER_SIZE) {
        return ELF64_STATUS_TRUNCATED;
    }

    if (image->program_header_count == 0 ||
        image->program_header_count >
            ELF64_MAX_PROGRAM_HEADERS ||
        image->load_segment_count == 0 ||
        image->load_segment_count >
            ELF64_MAX_LOAD_SEGMENTS ||
        image->load_segment_count >
            image->program_header_count) {

        return ELF64_STATUS_INVALID_FORMAT;
    }

    if (image->entry < ELF64_USER_MIN_ADDRESS ||
        image->entry >= ELF64_USER_ADDRESS_LIMIT) {

        return ELF64_STATUS_INVALID_ENTRY;
    }

    bool executable_entry_found = false;

    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        const elf64_load_segment_t *segment =
            &image->segments[i];

        /*
         * A parsed image stores only nonempty PT_LOAD
         * segments.
         */
        if (segment->memory_size == 0 ||
            segment->file_size > segment->memory_size) {

            return ELF64_STATUS_INVALID_SEGMENT;
        }

        /*
         * Revalidate the file-backed range.
         */
        if (segment->file_offset > image->file_size ||
            segment->file_size >
                image->file_size - segment->file_offset) {

            return ELF64_STATUS_TRUNCATED;
        }

        /*
         * Revalidate permissions.
         */
        if (segment->flags &
            ~(ELF64_PF_R | ELF64_PF_W | ELF64_PF_X)) {

            return ELF64_STATUS_UNSUPPORTED;
        }

        if (!(segment->flags & ELF64_PF_R)) {
            return ELF64_STATUS_UNSUPPORTED;
        }

        if ((segment->flags & ELF64_PF_W) &&
            (segment->flags & ELF64_PF_X)) {

            return ELF64_STATUS_PERMISSION_CONFLICT;
        }

        /*
         * Revalidate p_align.
         */
        if (segment->alignment > 1ULL) {

            if (!elf_is_power_of_two(segment->alignment)) {
                return ELF64_STATUS_INVALID_SEGMENT;
            }

            if (
                (segment->virtual_address &
                    (segment->alignment - 1ULL)) !=
                (segment->file_offset &
                    (segment->alignment - 1ULL))
            ) {
                return ELF64_STATUS_INVALID_SEGMENT;
            }
        }

        /*
         * Validate the address range without overflow.
         */
        if (segment->virtual_address <
                ELF64_USER_MIN_ADDRESS ||
            segment->virtual_address >=
                ELF64_USER_ADDRESS_LIMIT ||
            segment->memory_size >
                ELF64_USER_ADDRESS_LIMIT -
                    segment->virtual_address) {

            return ELF64_STATUS_INVALID_SEGMENT;
        }

        uint64_t expected_memory_end =
            segment->virtual_address +
            segment->memory_size;

        if (segment->memory_end != expected_memory_end) {
            return ELF64_STATUS_INVALID_SEGMENT;
        }

        /*
         * Recompute the page range instead of trusting
         * the cached values in elf64_load_segment_t.
         */
        uint64_t expected_page_start =
            segment->virtual_address & ~ELF64_PAGE_MASK;

        if (expected_memory_end >
            UINT64_MAX - ELF64_PAGE_MASK) {

            return ELF64_STATUS_INVALID_SEGMENT;
        }

        uint64_t expected_page_end =
            (expected_memory_end + ELF64_PAGE_MASK) &
            ~ELF64_PAGE_MASK;

        if (segment->page_start != expected_page_start ||
            segment->page_end != expected_page_end ||
            segment->page_end <= segment->page_start ||
            segment->page_start < ELF64_USER_MIN_ADDRESS ||
            segment->page_end > ELF64_USER_ADDRESS_LIMIT) {

            return ELF64_STATUS_INVALID_SEGMENT;
        }

        /*
         * Verify that the entry point still belongs to
         * an executable segment.
         */
        if ((segment->flags & ELF64_PF_X) &&
            image->entry >= segment->virtual_address &&
            image->entry < segment->memory_end) {

            executable_entry_found = true;
        }
    }

    if (!executable_entry_found) {
        return ELF64_STATUS_INVALID_ENTRY;
    }

    /*
     * Check byte-range overlap, page-level permission
     * compatibility, and the unique-page resource limit.
     */
    return elf_validate_segment_relationships(image);
}


/*
 * Build the final, sorted list of unique 4 KiB pages.
 *
 * The input must be a valid parsed image.
 */
elf64_status_t elf64_build_page_plan(
    const elf64_image_t *image,
    elf64_page_plan_entry_t *page_storage,
    uint64_t page_capacity,
    uint64_t *page_count_out
) {
    if (image == NULL ||
        page_storage == NULL ||
        page_count_out == NULL) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate everything before modifying the output array.
     */
    elf64_status_t status =
        elf_validate_page_plan_image(image);

    if (status != ELF64_STATUS_OK) {
        return status;
    }

    /*
     * Prepare segment ordering.
     */
    uint16_t sorted_indices[
        ELF64_MAX_LOAD_SEGMENTS
    ];

    elf_sort_segment_indices_by_page_start(
        image,
        sorted_indices
    );

    /*
     * Determine the exact number of unique pages.
     */
    uint64_t required_pages = 0;

    status = elf_count_unique_pages(
        image,
        sorted_indices,
        &required_pages
    );

    if (status != ELF64_STATUS_OK) {
        return status;
    }

    /*
     * Check capacity before writing anything.
     *
     * This preserves the caller's array and page count
     * if the supplied storage is too small.
     */
    if (required_pages > page_capacity) {
        return ELF64_STATUS_INSUFFICIENT_CAPACITY;
    }

    /*
     * Validate every input/output range before writing anything.
     *
     * Reject:
     *     - Page-plan output overlapping the parsed image.
     *     - Page-plan output overlapping the ELF file.
     *     - Page-plan output overlapping page_count_out.
     *     - page_count_out overlapping the parsed image.
     *     - page_count_out overlapping the ELF file.
     *
     * Use required_pages rather than page_capacity because
     * only the required entries will be written.
     */
    uint64_t storage_size =
        required_pages * sizeof(*page_storage);

    if (elf_ranges_overlap(
            page_storage,
            storage_size,
            image,
            sizeof(*image)
        ) ||
        elf_ranges_overlap(
            page_storage,
            storage_size,
            image->file_data,
            image->file_size
        ) ||
        elf_ranges_overlap(
            page_storage,
            storage_size,
            page_count_out,
            sizeof(*page_count_out)
        ) ||
        elf_ranges_overlap(
            page_count_out,
            sizeof(*page_count_out),
            image,
            sizeof(*image)
        ) ||
        elf_ranges_overlap(
            page_count_out,
            sizeof(*page_count_out),
            image->file_data,
            image->file_size
        )) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Produce the sorted union of all segment page ranges.
     *
     * Since overlapping page ranges were already checked,
     * every shared page has compatible permissions.
     */
    uint64_t written_pages = 0;
    uint64_t covered_end = 0;

    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        const elf64_load_segment_t *segment =
            &image->segments[sorted_indices[i]];

        uint64_t first_uncovered =
            segment->page_start;

        if (first_uncovered < covered_end) {
            first_uncovered = covered_end;
        }

        for (uint64_t virtual_address = first_uncovered;
             virtual_address < segment->page_end;
             virtual_address += ELF64_PAGE_SIZE) {

            elf64_page_plan_entry_t *entry =
                &page_storage[written_pages];

            entry->virtual_address = virtual_address;
            entry->segment_flags = segment->flags;
            entry->reserved = 0;

            written_pages++;
        }

        if (segment->page_end > covered_end) {
            covered_end = segment->page_end;
        }
    }

    /*
     * All failure conditions have been checked before
     * writing the array. Commit the resulting count last.
     */
    *page_count_out = written_pages;

    return ELF64_STATUS_OK;
}

/*
 * Determine whether two address ranges overlap.
 *
 * If either range would overflow the uintptr_t address space,
 * treat the ranges as overlapping and reject the operation.
 */
static bool elf_ranges_overlap(
    const void *first,
    uint64_t first_size,
    const void *second,
    uint64_t second_size
) {
    if (first_size == 0 || second_size == 0) {
        return false;
    }

    uintptr_t first_start = (uintptr_t)first;
    uintptr_t second_start = (uintptr_t)second;

    if (first_size >
        (uint64_t)(UINTPTR_MAX - first_start)) {

        return true;
    }

    if (second_size >
        (uint64_t)(UINTPTR_MAX - second_start)) {

        return true;
    }

    uintptr_t first_end =
        first_start + (uintptr_t)first_size;

    uintptr_t second_end =
        second_start + (uintptr_t)second_size;

    return first_start < second_end &&
        second_start < first_end;
}

/*
 * Validate the permission bits stored in a prepared page.
 */
static bool elf_prepared_page_flags_valid(
    uint32_t flags
) {
    if (flags & ~(ELF64_PF_R |
                  ELF64_PF_W |
                  ELF64_PF_X)) {

        return false;
    }

    if (!(flags & ELF64_PF_R)) {
        return false;
    }

    if ((flags & ELF64_PF_W) &&
        (flags & ELF64_PF_X)) {

        return false;
    }

    return true;
}

static void elf_prepared_page_swap(
    elf64_prepared_page_t *a,
    elf64_prepared_page_t *b
) {
    elf64_prepared_page_t temporary = *a;

    *a = *b;
    *b = temporary;
}


static void elf_prepared_page_sift_down(
    elf64_prepared_page_t *pages,
    uint64_t root,
    uint64_t heap_size
) {
    while (root < heap_size / 2) {

        uint64_t child = root * 2ULL + 1ULL;

        if (child + 1ULL < heap_size &&
            pages[child].physical_address <
                pages[child + 1ULL].physical_address) {

            child++;
        }

        if (pages[root].physical_address >=
            pages[child].physical_address) {

            break;
        }

        elf_prepared_page_swap(
            &pages[root],
            &pages[child]
        );

        root = child;
    }
}


/*
 * Sort prepared-page records by physical address.
 *
 * This operation is in-place and requires no heap allocation.
 */
static void elf_sort_prepared_pages_by_physical_address(
    elf64_prepared_page_t *pages,
    uint64_t count
) {
    if (count < 2) {
        return;
    }

    /*
     * Construct a max heap.
     */
    for (uint64_t start = count / 2;
         start > 0;) {

        start--;

        elf_prepared_page_sift_down(
            pages,
            start,
            count
        );
    }

    /*
     * Repeatedly move the largest element to the end.
     */
    for (uint64_t end = count;
         end > 1;) {

        elf_prepared_page_swap(
            &pages[0],
            &pages[end - 1]
        );

        end--;

        elf_prepared_page_sift_down(
            pages,
            0,
            end
        );
    }
}

/*
 * Release prepared physical pages.
 *
 * The function validates all records before freeing any frame.
 * It then sorts by physical address to detect duplicate frames.
 *
 * On partial failure, records for frames that could not be
 * released are compacted at the beginning of the array.
 */
elf64_status_t elf64_release_prepared_pages(
    elf64_prepared_page_t *page_storage,
    uint64_t *page_count_io
) {
    if (page_count_io == NULL) {
        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    uint64_t page_count = *page_count_io;

    if (page_count > ELF64_MAX_IMAGE_PAGES) {
        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    if (page_count == 0) {
        return ELF64_STATUS_OK;
    }

    if (page_storage == NULL) {
        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    uint64_t storage_size =
        page_count * sizeof(*page_storage);

    /*
     * The count pointer must not alias the records being
     * released.
     */
    if (elf_ranges_overlap(
            page_storage,
            storage_size,
            page_count_io,
            sizeof(*page_count_io)
        )) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate every record before freeing any frame.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        const elf64_prepared_page_t *page =
            &page_storage[i];

        if (page->physical_address == 0 ||
            (page->physical_address &
                (PMM_PAGE_SIZE - 1ULL)) != 0) {

            return ELF64_STATUS_INVALID_ARGUMENT;
        }

        if (page->virtual_address <
                ELF64_USER_MIN_ADDRESS ||
            page->virtual_address >=
                ELF64_USER_ADDRESS_LIMIT ||
            (page->virtual_address &
                ELF64_PAGE_MASK) != 0 ||
            page->virtual_address >
                ELF64_USER_ADDRESS_LIMIT -
                    ELF64_PAGE_SIZE) {

            return ELF64_STATUS_INVALID_ARGUMENT;
        }

        if (!elf_prepared_page_flags_valid(
                page->segment_flags
            )) {

            return ELF64_STATUS_INVALID_ARGUMENT;
        }

        if (page->reserved != 0) {
            return ELF64_STATUS_INVALID_ARGUMENT;
        }
    }

    /*
     * Detect duplicate physical addresses before freeing
     * anything. Heapsort avoids quadratic duplicate checking.
     *
     * Release is destructive, so changing the record order
     * is acceptable.
     */
    elf_sort_prepared_pages_by_physical_address(
        page_storage,
        page_count
    );

    for (uint64_t i = 1; i < page_count; i++) {

        if (page_storage[i - 1].physical_address ==
            page_storage[i].physical_address) {

            return ELF64_STATUS_INVALID_ARGUMENT;
        }
    }

    /*
     * Free all frames, retaining records for failed releases.
     */
    uint64_t remaining = 0;

    for (uint64_t i = 0; i < page_count; i++) {

        if (!pmm_free_frame(
                page_storage[i].physical_address
            )) {

            if (remaining != i) {
                page_storage[remaining] = page_storage[i];
            }

            remaining++;
        }
    }

    /*
     * Clear records that no longer represent owned frames.
     */
    if (remaining < page_count) {

        memset(
            &page_storage[remaining],
            0,
            (size_t)(
                (page_count - remaining) *
                sizeof(*page_storage)
            )
        );
    }

    *page_count_io = remaining;

    if (remaining != 0) {
        return ELF64_STATUS_CLEANUP_FAILED;
    }

    return ELF64_STATUS_OK;
}

/*
 * Find a prepared page by its aligned virtual address.
 *
 * The input array must be sorted by virtual_address.
 */
static elf64_prepared_page_t *elf_find_prepared_page(
    elf64_prepared_page_t *pages,
    uint64_t page_count,
    uint64_t virtual_address
) {
    uint64_t low = 0;
    uint64_t high = page_count;

    while (low < high) {

        uint64_t middle =
            low + (high - low) / 2ULL;

        uint64_t candidate =
            pages[middle].virtual_address;

        if (candidate < virtual_address) {

            low = middle + 1ULL;

        } else if (candidate > virtual_address) {

            high = middle;

        } else {

            return &pages[middle];
        }
    }

    return NULL;
}

/*
 * Roll back all frames allocated by one preparation operation.
 */
static elf64_status_t elf_rollback_prepared_pages(
    elf64_prepared_page_t *page_storage,
    uint64_t allocated_count,
    uint64_t *page_count_out,
    elf64_status_t original_status
) {
    uint64_t remaining = allocated_count;

    elf64_status_t release_status =
        elf64_release_prepared_pages(
            page_storage,
            &remaining
        );

    if (release_status != ELF64_STATUS_OK) {

        /*
         * Do not lose track of frames that remain allocated.
         */
        *page_count_out = remaining;

        return ELF64_STATUS_CLEANUP_FAILED;
    }

    /*
     * All allocated frames were released.
     * The caller's page_count_out remains unchanged.
     */
    return original_status;
}

/*
 * Allocate and initialize all physical pages required by
 * a validated ELF image.
 *
 * The final result contains one record per unique virtual page.
 *
 * All pages are zeroed before any file-backed segment data
 * is copied into them. This preserves BSS and page gaps,
 * including when multiple segments share one physical page.
 */
elf64_status_t elf64_prepare_pages(
    const elf64_image_t *image,
    elf64_prepared_page_t *page_storage,
    uint64_t page_capacity,
    uint64_t *page_count_out
) {
    if (image == NULL ||
        page_storage == NULL ||
        page_count_out == NULL) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate the parsed image again.
     *
     * The public image structure may have been modified
     * since elf64_parse() returned.
     */
    elf64_status_t status =
        elf_validate_page_plan_image(image);

    if (status != ELF64_STATUS_OK) {
        return status;
    }

    /*
     * Build the deterministic segment ordering.
     */
    uint16_t sorted_indices[
        ELF64_MAX_LOAD_SEGMENTS
    ];

    elf_sort_segment_indices_by_page_start(
        image,
        sorted_indices
    );

    /*
     * Determine the exact number of unique pages.
     */
    uint64_t required_pages = 0;

    status = elf_count_unique_pages(
        image,
        sorted_indices,
        &required_pages
    );

    if (status != ELF64_STATUS_OK) {
        return status;
    }

    if (required_pages == 0) {
        return ELF64_STATUS_INVALID_FORMAT;
    }

    /*
     * Check capacity before allocating anything.
     */
    if (required_pages > page_capacity) {
        return ELF64_STATUS_INSUFFICIENT_CAPACITY;
    }

    uint64_t storage_size =
        required_pages * sizeof(*page_storage);

    /*
     * Prevent output from overwriting:
     * - The image descriptor.
     * - Its borrowed ELF file data.
     * - The output count.
     *
     * Also prevent the count from aliasing the input data.
     */
    if (elf_ranges_overlap(
            page_storage,
            storage_size,
            image,
            sizeof(*image)
        ) ||
        elf_ranges_overlap(
            page_storage,
            storage_size,
            image->file_data,
            image->file_size
        ) ||
        elf_ranges_overlap(
            page_storage,
            storage_size,
            page_count_out,
            sizeof(*page_count_out)
        ) ||
        elf_ranges_overlap(
            page_count_out,
            sizeof(*page_count_out),
            image,
            sizeof(*image)
        ) ||
        elf_ranges_overlap(
            page_count_out,
            sizeof(*page_count_out),
            image->file_data,
            image->file_size
        )) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    uint64_t allocated_count = 0;
    uint64_t covered_end = 0;

    /*
     * PHASE 1:
     *
     * Allocate one frame per unique virtual page.
     *
     * The segment page ranges are already sorted.
     * Shared pages are skipped using covered_end.
     */
    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        const elf64_load_segment_t *segment =
            &image->segments[sorted_indices[i]];

        uint64_t first_uncovered =
            segment->page_start;

        if (first_uncovered < covered_end) {
            first_uncovered = covered_end;
        }

        for (uint64_t virtual_address = first_uncovered;
             virtual_address < segment->page_end;
             virtual_address += ELF64_PAGE_SIZE) {

            /*
             * Allocate a physical frame.
             */
            uint64_t physical_address =
                pmm_alloc_frame();

            if (physical_address == 0) {

                return elf_rollback_prepared_pages(
                    page_storage,
                    allocated_count,
                    page_count_out,
                    ELF64_STATUS_OUT_OF_MEMORY
                );
            }

            /*
             * Record ownership immediately.
             *
             * If HHDM conversion fails, rollback must still
             * know about this newly allocated frame.
             */
            elf64_prepared_page_t *page =
                &page_storage[allocated_count];

            page->virtual_address = virtual_address;
            page->physical_address = physical_address;
            page->segment_flags = segment->flags;
            page->reserved = 0;

            allocated_count++;

            /*
             * Obtain the kernel-accessible HHDM alias.
             */
            void *page_virtual =
                pmm_frame_to_virt(physical_address);

            if (page_virtual == NULL) {

                return elf_rollback_prepared_pages(
                    page_storage,
                    allocated_count,
                    page_count_out,
                    ELF64_STATUS_MEMORY_ACCESS_FAILED
                );
            }

            /*
             * Explicitly zero the complete frame.
             *
             * pmm_alloc_frame() currently zeroes its frames,
             * but this loader does not rely on that internal
             * implementation detail.
             */
            memset(
                page_virtual,
                0,
                (size_t)ELF64_PAGE_SIZE
            );
        }

        if (segment->page_end > covered_end) {
            covered_end = segment->page_end;
        }
    }

    /*
     * Internal invariant:
     * The number of allocated frames must equal the
     * previously calculated unique-page count.
     */
    if (allocated_count != required_pages) {

        return elf_rollback_prepared_pages(
            page_storage,
            allocated_count,
            page_count_out,
            ELF64_STATUS_INVALID_FORMAT
        );
    }

    /*
     * PHASE 2:
     *
     * Copy file-backed segment bytes into the prepared pages.
     *
     * Every destination frame is already zeroed.
     * BSS and unused page bytes therefore remain zero.
     */
    for (uint16_t i = 0;
         i < image->load_segment_count;
         i++) {

        const elf64_load_segment_t *segment =
            &image->segments[i];

        uint64_t copied = 0;

        while (copied < segment->file_size) {

            /*
             * Determine the current destination virtual address.
             */
            if (copied >
                UINT64_MAX - segment->virtual_address) {

                return elf_rollback_prepared_pages(
                    page_storage,
                    allocated_count,
                    page_count_out,
                    ELF64_STATUS_INVALID_SEGMENT
                );
            }

            uint64_t current_virtual_address =
                segment->virtual_address + copied;

            uint64_t page_address =
                current_virtual_address & ~ELF64_PAGE_MASK;

            uint64_t page_offset =
                current_virtual_address & ELF64_PAGE_MASK;

            /*
             * Copy no more than the remainder of this page.
             */
            uint64_t chunk_size =
                ELF64_PAGE_SIZE - page_offset;

            uint64_t remaining =
                segment->file_size - copied;

            if (chunk_size > remaining) {
                chunk_size = remaining;
            }

            /*
             * Locate the already allocated physical frame.
             */
            elf64_prepared_page_t *page =
                elf_find_prepared_page(
                    page_storage,
                    allocated_count,
                    page_address
                );

            if (page == NULL) {

                return elf_rollback_prepared_pages(
                    page_storage,
                    allocated_count,
                    page_count_out,
                    ELF64_STATUS_INVALID_SEGMENT
                );
            }

            /*
             * Obtain its HHDM alias.
             */
            uint8_t *destination =
                (uint8_t *)pmm_frame_to_virt(
                    page->physical_address
                );

            if (destination == NULL) {

                return elf_rollback_prepared_pages(
                    page_storage,
                    allocated_count,
                    page_count_out,
                    ELF64_STATUS_MEMORY_ACCESS_FAILED
                );
            }

            /*
             * Determine the corresponding source-file offset.
             */
            if (copied >
                UINT64_MAX - segment->file_offset) {

                return elf_rollback_prepared_pages(
                    page_storage,
                    allocated_count,
                    page_count_out,
                    ELF64_STATUS_INVALID_SEGMENT
                );
            }

            uint64_t source_offset =
                segment->file_offset + copied;

            /*
             * Copy only the bytes belonging to this page.
             */
            memcpy(
                destination + page_offset,
                image->file_data + source_offset,
                (size_t)chunk_size
            );

            copied += chunk_size;
        }
    }

    /*
     * PHASE 3:
     *
     * Commit ownership information to the caller.
     *
     * No page-table mappings have been created.
     */
    *page_count_out = allocated_count;

    return ELF64_STATUS_OK;
}

/*
 * Parse and validate an ELF64 executable.
 */
elf64_status_t elf64_parse(
    const void *file_data,
    uint64_t file_size,
    elf64_image_t *image_out
) {

    if (file_data == NULL ||
        image_out == NULL) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    /*
     * The parser promises not to modify the input ELF bytes.
     *
     * The final image_out assignment would violate that promise
     * if the output object overlapped the input file.
     */
    if (elf_ranges_overlap(
            file_data,
            file_size,
            image_out,
            sizeof(*image_out)
        )) {

        return ELF64_STATUS_INVALID_ARGUMENT;
    }

    if (file_size < ELF_HEADER_SIZE) {
        return ELF64_STATUS_TRUNCATED;
    }

    const uint8_t *bytes =
        (const uint8_t *)file_data;

    /*
     * Use a local result so failed parsing never exposes
     * a partially initialized image to the caller.
     */
    elf64_image_t image = {0};

    /*
     * Check ELF magic.
     */
    for (uint32_t i = 0; i < 4; i++) {

        if (bytes[i] != elf_magic[i]) {
            return ELF64_STATUS_INVALID_FORMAT;
        }
    }

    /*
     * Check ELF identification.
     */
    if (bytes[ELF_EI_CLASS] != ELF_CLASS_64 ||
        bytes[ELF_EI_DATA] != ELF_DATA_LSB ||
        bytes[ELF_EI_VERSION] != ELF_VERSION_1) {

        return ELF64_STATUS_UNSUPPORTED;
    }

    /*
     * The first HUA executable ABI uses System V ELF
     * identification and ABI version zero.
     */
    if (bytes[ELF_EI_OSABI] != ELF_OSABI_SYSV ||
        bytes[ELF_EI_ABIVERSION] != 0) {

        return ELF64_STATUS_UNSUPPORTED;
    }

    /*
     * The remaining ELF identification bytes are reserved.
     * Reject nonzero values rather than accepting an ABI
     * extension whose meaning HUA does not understand.
     */
    for (uint32_t i = ELF_EI_PAD_START; i < 16; i++) {

        if (bytes[i] != 0) {
            return ELF64_STATUS_UNSUPPORTED;
        }
    }

    /*
     * Read and validate the main ELF header.
     */
    uint16_t object_type =
        elf_read_u16(&bytes[ELF_E_TYPE]);

    uint16_t machine =
        elf_read_u16(&bytes[ELF_E_MACHINE]);

    uint32_t version =
        elf_read_u32(&bytes[ELF_E_VERSION]);

    uint64_t entry =
        elf_read_u64(&bytes[ELF_E_ENTRY]);

    uint64_t program_header_offset =
        elf_read_u64(&bytes[ELF_E_PHOFF]);

    uint32_t elf_flags =
        elf_read_u32(&bytes[ELF_E_FLAGS]);

    uint16_t header_size =
        elf_read_u16(&bytes[ELF_E_EHSIZE]);

    uint16_t program_header_size =
        elf_read_u16(&bytes[ELF_E_PHENTSIZE]);

    uint16_t program_header_count =
        elf_read_u16(&bytes[ELF_E_PHNUM]);

    if (object_type != ELF_ET_EXEC ||
        machine != ELF_EM_X86_64) {

        return ELF64_STATUS_UNSUPPORTED;
    }

    if (version != ELF_VERSION_1 ||
        header_size != ELF_HEADER_SIZE ||
        elf_flags != 0) {

        return ELF64_STATUS_INVALID_FORMAT;
    }

    if (program_header_size !=
        ELF_PROGRAM_HEADER_SIZE) {

        return ELF64_STATUS_INVALID_FORMAT;
    }

    if (program_header_count == 0) {
        return ELF64_STATUS_INVALID_FORMAT;
    }

    /*
    * Extended program-header numbering is not supported.
    */
    if (program_header_count == 0xFFFFU) {
        return ELF64_STATUS_UNSUPPORTED;
    }

    if (program_header_count >
        ELF64_MAX_PROGRAM_HEADERS) {

        return ELF64_STATUS_LIMIT_EXCEEDED;
    }

    /*
     * The program-header table must follow the ELF header.
     */
    if (program_header_offset < ELF_HEADER_SIZE) {
        return ELF64_STATUS_INVALID_FORMAT;
    }

    /*
     * Validate the complete table without overflowing
     * program_header_offset + count * entry_size.
     */
    if (program_header_offset > file_size ||
        program_header_count >
            (file_size - program_header_offset) /
            ELF_PROGRAM_HEADER_SIZE) {

        return ELF64_STATUS_TRUNCATED;
    }

    /*
     * The initial entry point must be inside the supported
     * lower-half user address range.
     */
    if (entry < ELF64_USER_MIN_ADDRESS ||
        entry >= ELF64_USER_ADDRESS_LIMIT) {

        return ELF64_STATUS_INVALID_ENTRY;
    }

    image.file_data = bytes;
    image.file_size = file_size;
    image.entry = entry;
    image.program_header_count = program_header_count;

    bool executable_entry_found = false;
    bool gnu_stack_seen = false;

    /*
     * Parse every program header.
     */
    for (uint16_t i = 0;
         i < program_header_count;
         i++) {

        uint64_t header_offset =
            program_header_offset +
            (uint64_t)i * ELF_PROGRAM_HEADER_SIZE;

        const uint8_t *program_header =
            &bytes[header_offset];

        uint32_t type =
            elf_read_u32(
                &program_header[ELF_PH_TYPE]
            );

        uint32_t flags =
            elf_read_u32(
                &program_header[ELF_PH_FLAGS]
            );

        uint64_t file_offset =
            elf_read_u64(
                &program_header[ELF_PH_OFFSET]
            );

        uint64_t virtual_address =
            elf_read_u64(
                &program_header[ELF_PH_VADDR]
            );

        uint64_t file_bytes =
            elf_read_u64(
                &program_header[ELF_PH_FILESZ]
            );

        uint64_t memory_bytes =
            elf_read_u64(
                &program_header[ELF_PH_MEMSZ]
            );

        uint64_t alignment =
            elf_read_u64(
                &program_header[ELF_PH_ALIGN]
            );

        /*
         * Loadable memory.
         */
        if (type == ELF_PT_LOAD) {

            if (image.load_segment_count >=
                ELF64_MAX_LOAD_SEGMENTS) {

                return ELF64_STATUS_LIMIT_EXCEEDED;
            }

            elf64_load_segment_t segment = {0};

            elf64_status_t status =
                elf_validate_load_segment(
                    bytes,
                    file_size,
                    file_offset,
                    virtual_address,
                    file_bytes,
                    memory_bytes,
                    alignment,
                    flags,
                    &segment
                );

            if (status != ELF64_STATUS_OK) {
                return status;
            }

            /*
             * A zero-sized segment contributes nothing to
             * executable memory or entry-point validation.
             */
            if (memory_bytes == 0) {
                continue;
            }

            if ((flags & ELF64_PF_X) &&
                entry >= virtual_address &&
                entry < segment.memory_end) {

                executable_entry_found = true;
            }

            image.segments[
                image.load_segment_count
            ] = segment;

            image.load_segment_count++;

            continue;
        }

        /*
         * A dynamic linker is not implemented.
         */
        if (type == ELF_PT_DYNAMIC ||
            type == ELF_PT_INTERP ||
            type == ELF_PT_SHLIB ||
            type == ELF_PT_TLS ||
            type == ELF_PT_GNU_RELRO) {

            return ELF64_STATUS_UNSUPPORTED;
        }

        /*
         * The user stack is always mapped writable and
         * non-executable by HUA.
         *
         * Do not accept an executable-stack request.
         */
        if (type == ELF_PT_GNU_STACK) {

            if (gnu_stack_seen) {
                return ELF64_STATUS_INVALID_FORMAT;
            }

            gnu_stack_seen = true;

            if (flags & ELF64_PF_X) {
                return ELF64_STATUS_UNSUPPORTED;
            }

            if (flags & ~(ELF64_PF_R |
                          ELF64_PF_W |
                          ELF64_PF_X)) {

                return ELF64_STATUS_UNSUPPORTED;
            }

            continue;
        }

        /*
         * Other metadata program headers are not needed
         * by this initial static executable loader.
         *
         * They do not define mapped user memory here.
         */
        if (type == ELF_PT_NULL) {
            continue;
        }
    }

    /*
     * At least one real loadable segment is required.
     */
    if (image.load_segment_count == 0) {
        return ELF64_STATUS_INVALID_FORMAT;
    }

    /*
     * The entry must belong to an executable PT_LOAD segment.
     */
    if (!executable_entry_found) {
        return ELF64_STATUS_INVALID_ENTRY;
    }

    /*
     * Validate relationships between all loadable segments.
     */
    elf64_status_t relationship_status =
        elf_validate_segment_relationships(&image);

    if (relationship_status != ELF64_STATUS_OK) {
        return relationship_status;
    }

    /*
     * Parsing succeeded.
     *
     * This is the only point at which the caller's output
     * is modified.
     */
    *image_out = image;

    return ELF64_STATUS_OK;
}


/*
 * Convert a parser status to diagnostic text.
 */
const char *elf64_status_string(
    elf64_status_t status
) {
    switch (status) {

        case ELF64_STATUS_OK:
            return "success";

        case ELF64_STATUS_INVALID_ARGUMENT:
            return "invalid argument";

        case ELF64_STATUS_TRUNCATED:
            return "truncated ELF image";

        case ELF64_STATUS_INVALID_FORMAT:
            return "invalid ELF format";

        case ELF64_STATUS_UNSUPPORTED:
            return "unsupported ELF feature";

        case ELF64_STATUS_LIMIT_EXCEEDED:
            return "ELF resource limit exceeded";

        case ELF64_STATUS_INVALID_SEGMENT:
            return "invalid load segment";

        case ELF64_STATUS_PERMISSION_CONFLICT:
            return "segment permission conflict";

        case ELF64_STATUS_INVALID_ENTRY:
            return "invalid executable entry point";

        case ELF64_STATUS_INSUFFICIENT_CAPACITY:
            return "page-plan output capacity is insufficient";

        case ELF64_STATUS_OUT_OF_MEMORY:
            return "physical memory allocation failed";

        case ELF64_STATUS_MEMORY_ACCESS_FAILED:
            return "physical frame HHDM access failed";

        case ELF64_STATUS_CLEANUP_FAILED:
            return "physical frame cleanup failed";

        default:
            return "unknown ELF parser error";
    }
}
