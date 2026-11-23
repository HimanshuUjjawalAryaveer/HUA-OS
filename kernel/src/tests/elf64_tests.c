#include <stdint.h>
#include <stdbool.h>
#include "exec/elf64.h"
#include "mm/pmm.h"
#include <stddef.h>

#include "tests/elf64_tests.h"

#define TEST_ELF_BUFFER_SIZE  0x4000U

#define TEST_ELF_HEADER_SIZE  64U
#define TEST_ELF_PH_SIZE      56U
#define TEST_ELF_PH_OFFSET    64U

#define TEST_ELF_PT_LOAD      1U
#define TEST_ELF_PT_DYNAMIC   2U
#define TEST_ELF_PT_GNU_STACK 0x6474E551U

#define TEST_ELF_ET_EXEC      2U
#define TEST_ELF_ET_DYN       3U

#define TEST_ELF_EM_X86_64    62U

#define TEST_ELF_PF_X         0x1U
#define TEST_ELF_PF_W         0x2U
#define TEST_ELF_PF_R         0x4U

#define TEST_ELF_VALID_ENTRY  0x400000ULL
#define TEST_ELF_VALID_VADDR  0x400000ULL
#define TEST_ELF_VALID_OFFSET 0x1000ULL

/*
 * Static test storage avoids consuming several pages of the
 * bootstrap thread's stack.
 *
 * Tests execute sequentially during kernel initialization.
 */
_Alignas(16) static uint8_t test_buffer[
    TEST_ELF_BUFFER_SIZE
];

static elf64_image_t test_output;

static elf64_image_t test_output_snapshot;

static elf64_page_plan_entry_t test_plan_pages[8];

static elf64_prepared_page_t test_prepared_pages[8];

/*
 * Write an unsigned 16-bit little-endian value.
 */
static void test_write_u16(
    uint8_t *destination,
    uint16_t value
) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}


/*
 * Write an unsigned 32-bit little-endian value.
 */
static void test_write_u32(
    uint8_t *destination,
    uint32_t value
) {
    for (uint32_t i = 0; i < 4; i++) {
        destination[i] =
            (uint8_t)(value >> (i * 8U));
    }
}


/*
 * Write an unsigned 64-bit little-endian value.
 */
static void test_write_u64(
    uint8_t *destination,
    uint64_t value
) {
    for (uint32_t i = 0; i < 8; i++) {
        destination[i] =
            (uint8_t)(value >> (i * 8U));
    }
}


/*
 * Write one synthetic ELF64 program header.
 *
 * index is the program-header index.
 */
static void test_write_program_header(
    uint8_t *image,
    uint32_t index,
    uint32_t type,
    uint32_t flags,
    uint64_t file_offset,
    uint64_t virtual_address,
    uint64_t file_size,
    uint64_t memory_size,
    uint64_t alignment
) {
    uint64_t offset =
        TEST_ELF_PH_OFFSET +
        (uint64_t)index * TEST_ELF_PH_SIZE;

    uint8_t *header = &image[offset];

    test_write_u32(&header[0], type);
    test_write_u32(&header[4], flags);

    test_write_u64(&header[8], file_offset);
    test_write_u64(&header[16], virtual_address);

    /*
     * p_paddr is irrelevant to HUA's virtual-memory loader.
     */
    test_write_u64(&header[24], 0);

    test_write_u64(&header[32], file_size);
    test_write_u64(&header[40], memory_size);
    test_write_u64(&header[48], alignment);
}


/*
 * Build a valid, minimal ELF64 executable.
 *
 * It has:
 *     - One executable/readable PT_LOAD.
 *     - A four-byte file-backed region.
 *     - A larger memory size, exercising BSS zero-fill.
 *     - A valid entry point.
 */
static void test_build_valid_image(void) {

    for (uint32_t i = 0;
         i < TEST_ELF_BUFFER_SIZE;
         i++) {

        test_buffer[i] = 0;
    }

    /*
     * ELF identification.
     */
    test_buffer[0] = 0x7F;
    test_buffer[1] = 'E';
    test_buffer[2] = 'L';
    test_buffer[3] = 'F';

    test_buffer[4] = 2;  /* ELFCLASS64 */
    test_buffer[5] = 1;  /* ELFDATA2LSB */
    test_buffer[6] = 1;  /* EV_CURRENT */
    test_buffer[7] = 0;  /* System V ABI */
    test_buffer[8] = 0;  /* ABI version */

    /*
     * ELF header.
     */
    test_write_u16(
        &test_buffer[16],
        TEST_ELF_ET_EXEC
    );

    test_write_u16(
        &test_buffer[18],
        TEST_ELF_EM_X86_64
    );

    test_write_u32(
        &test_buffer[20],
        1
    );

    test_write_u64(
        &test_buffer[24],
        TEST_ELF_VALID_ENTRY
    );

    test_write_u64(
        &test_buffer[32],
        TEST_ELF_PH_OFFSET
    );

    test_write_u32(
        &test_buffer[48],
        0
    );

    test_write_u16(
        &test_buffer[52],
        TEST_ELF_HEADER_SIZE
    );

    test_write_u16(
        &test_buffer[54],
        TEST_ELF_PH_SIZE
    );

    test_write_u16(
        &test_buffer[56],
        1
    );

    /*
     * One valid executable PT_LOAD.
     *
     * Filesz = 4.
     * Memsz  = 4096.
     *
     * The remaining bytes must be zero-filled by a future
     * image loader.
     */
    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        TEST_ELF_VALID_OFFSET,
        TEST_ELF_VALID_VADDR,
        4,
        0x1000,
        0x1000
    );

    test_buffer[TEST_ELF_VALID_OFFSET + 0] = 0x90;
    test_buffer[TEST_ELF_VALID_OFFSET + 1] = 0x90;
    test_buffer[TEST_ELF_VALID_OFFSET + 2] = 0x90;
    test_buffer[TEST_ELF_VALID_OFFSET + 3] = 0xC3;
}

/*
 * Parse an input and require the expected failure status.
 *
 * Verify that the complete output object remains unchanged.
 * This includes fields and padding bytes.
 */
static bool test_expect_status(
    const uint8_t *bytes,
    uint64_t size,
    elf64_status_t expected
) {
    uint8_t *output_bytes =
        (uint8_t *)&test_output;

    uint8_t *snapshot_bytes =
        (uint8_t *)&test_output_snapshot;

    /*
     * Fill every byte with a recognizable pattern.
     */
    for (size_t i = 0;
         i < sizeof(test_output);
         i++) {

        output_bytes[i] =
            (uint8_t)(0xA5U ^ (uint8_t)(i * 37U));

        snapshot_bytes[i] = output_bytes[i];
    }

    elf64_status_t actual =
        elf64_parse(
            bytes,
            size,
            &test_output
        );

    if (actual != expected) {
        return false;
    }

    /*
     * Compare the complete output object.
     */
    for (size_t i = 0;
         i < sizeof(test_output);
         i++) {

        if (output_bytes[i] != snapshot_bytes[i]) {
            return false;
        }
    }

    return true;
}

/*
 * Test 1:
 * A valid executable is accepted.
 */
static bool test_valid_executable(void) {

    test_build_valid_image();

    elf64_status_t status =
        elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        );

    if (status != ELF64_STATUS_OK) {
        return false;
    }

    if (test_output.file_data != test_buffer ||
        test_output.file_size != sizeof(test_buffer) ||
        test_output.entry != TEST_ELF_VALID_ENTRY ||
        test_output.program_header_count != 1 ||
        test_output.load_segment_count != 1) {

        return false;
    }

    const elf64_load_segment_t *segment =
        &test_output.segments[0];

    if (segment->file_offset != TEST_ELF_VALID_OFFSET ||
        segment->virtual_address != TEST_ELF_VALID_VADDR ||
        segment->file_size != 4 ||
        segment->memory_size != 0x1000 ||
        segment->memory_end != 0x401000 ||
        segment->page_start != 0x400000 ||
        segment->page_end != 0x401000 ||
        segment->flags !=
            (TEST_ELF_PF_R | TEST_ELF_PF_X)) {

        return false;
    }

    return true;
}


/*
 * Test 2:
 * Two adjacent segments may share a page when their
 * page-level permissions are identical.
 */
static bool test_compatible_shared_page(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        2
    );

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        0x100,
        0x100,
        1
    );

    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1200,
        0x400100,
        0x100,
        0x100,
        1
    );

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    return test_output.load_segment_count == 2 &&
        test_output.segments[0].page_start ==
            test_output.segments[1].page_start &&
        test_output.segments[0].page_end ==
            test_output.segments[1].page_end;
}


/*
 * Test 3:
 * Segments sharing a page must not request incompatible
 * writable/executable permissions.
 */
static bool test_conflicting_shared_page(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        2
    );

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        0x100,
        0x100,
        1
    );

    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_W,
        0x1200,
        0x400100,
        0x100,
        0x100,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_PERMISSION_CONFLICT
    );
}


/*
 * Test 4:
 * Overlapping actual segment memory ranges are rejected.
 */
static bool test_overlapping_memory_ranges(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        2
    );

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        0x100,
        0x100,
        1
    );

    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1200,
        0x400080,
        0x100,
        0x100,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_SEGMENT
    );
}


/*
 * Test 5:
 * Writable and executable permissions on one segment
 * are rejected.
 */
static bool test_writable_executable_segment(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R |
            TEST_ELF_PF_W |
            TEST_ELF_PF_X,
        0x1000,
        0x400000,
        4,
        0x1000,
        0x1000
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_PERMISSION_CONFLICT
    );
}


/*
 * Test 6:
 * An entry point outside every executable segment is rejected.
 */
static bool test_entry_outside_executable_segment(void) {

    test_build_valid_image();

    test_write_u64(
        &test_buffer[24],
        0x500000
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_ENTRY
    );
}


/*
 * Test 7:
 * A non-executable load segment cannot contain the entry.
 */
static bool test_entry_in_non_executable_segment(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R,
        0x1000,
        0x400000,
        4,
        0x1000,
        0x1000
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_ENTRY
    );
}


/*
 * Test 8:
 * Executable memory must be readable under the initial
 * HUA page-permission model.
 */
static bool test_segment_without_read_permission(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_X,
        0x1000,
        0x400000,
        4,
        0x1000,
        0x1000
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_UNSUPPORTED
    );
}


/*
 * Test 9:
 * Unknown segment permission bits are rejected.
 */
static bool test_unknown_segment_flags(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X | 0x8U,
        0x1000,
        0x400000,
        4,
        0x1000,
        0x1000
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_UNSUPPORTED
    );
}

/*
 * Test 10:
 * Truncated file-backed segment data is rejected.
 */
static bool test_truncated_segment_data(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        (uint64_t)TEST_ELF_BUFFER_SIZE - 2ULL,
        0x400000,
        4,
        0x1000,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_TRUNCATED
    );
}

/*
 * Test 11:
 * Filesz greater than Memsz is invalid.
 */
static bool test_file_size_exceeds_memory_size(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        8,
        4,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_SEGMENT
    );
}


/*
 * Test 12:
 * ELF alignment must be zero, one, or a power of two.
 */
static bool test_invalid_alignment(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        4,
        0x1000,
        0x1800
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_SEGMENT
    );
}


/*
 * Test 13:
 * p_offset and p_vaddr must be congruent modulo p_align.
 */
static bool test_alignment_congruence(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1001,
        0x400000,
        4,
        0x1000,
        0x1000
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_SEGMENT
    );
}


/*
 * Test 14:
 * Segments below the reserved low user-address region
 * are rejected.
 */
static bool test_low_user_address(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x8000,
        4,
        0x1000,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_SEGMENT
    );
}


/*
 * Test 15:
 * A segment crossing the lower-half user-address limit
 * is rejected.
 */
static bool test_user_address_limit(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        ELF64_USER_ADDRESS_LIMIT - 0x1000,
        4,
        0x2000,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_SEGMENT
    );
}


/*
 * Test 16:
 * Dynamic linking is explicitly unsupported.
 */
static bool test_dynamic_segment_rejected(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        2
    );

    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_DYNAMIC,
        TEST_ELF_PF_R,
        0x1200,
        0x400100,
        16,
        16,
        8
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_UNSUPPORTED
    );
}


/*
 * Test 17:
 * An executable user-stack request is rejected.
 */
static bool test_executable_stack_rejected(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        2
    );

    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_GNU_STACK,
        TEST_ELF_PF_R |
            TEST_ELF_PF_W |
            TEST_ELF_PF_X,
        0,
        0,
        0,
        0,
        16
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_UNSUPPORTED
    );
}


/*
 * Test 18:
 * ET_DYN is not accepted by the initial loader.
 */
static bool test_dynamic_object_type_rejected(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[16],
        TEST_ELF_ET_DYN
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_UNSUPPORTED
    );
}


/*
 * Test 19:
 * Wrong ELF class is rejected.
 */
static bool test_wrong_elf_class(void) {

    test_build_valid_image();

    test_buffer[4] = 1;

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_UNSUPPORTED
    );
}


/*
 * Test 20:
 * Wrong byte order is rejected.
 */
static bool test_wrong_byte_order(void) {

    test_build_valid_image();

    test_buffer[5] = 2;

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_UNSUPPORTED
    );
}


/*
 * Test 21:
 * A truncated program-header table is rejected.
 */
static bool test_truncated_program_header_table(void) {

    test_build_valid_image();

    return test_expect_status(
        test_buffer,
        100,
        ELF64_STATUS_TRUNCATED
    );
}


/*
 * Test 22:
 * A program-header offset outside the image is rejected.
 */
static bool test_program_header_offset_out_of_bounds(void) {

    test_build_valid_image();

    test_write_u64(
        &test_buffer[32],
        UINT64_MAX - 8ULL
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_TRUNCATED
    );
}


/*
 * Test 23:
 * Too many program headers are rejected before parsing
 * the table.
 */
static bool test_program_header_count_limit(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        ELF64_MAX_PROGRAM_HEADERS + 1U
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_LIMIT_EXCEEDED
    );
}


/*
 * Test 24:
 * Excessive image memory consumption is rejected.
 */
static bool test_image_page_limit(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        4,
        (ELF64_MAX_IMAGE_PAGES + 1ULL) *
            ELF64_PAGE_SIZE,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_LIMIT_EXCEEDED
    );
}


/*
 * Test 25:
 * Too many PT_LOAD segments are rejected.
 */
static bool test_load_segment_count_limit(void) {

    test_build_valid_image();

    const uint16_t count =
        ELF64_MAX_LOAD_SEGMENTS + 1U;

    test_write_u16(
        &test_buffer[56],
        count
    );

    for (uint16_t i = 0; i < count; i++) {

        uint64_t virtual_address =
            0x400000ULL +
            (uint64_t)i * 0x2000ULL;

        test_write_program_header(
            test_buffer,
            i,
            TEST_ELF_PT_LOAD,
            TEST_ELF_PF_R | ELF64_PF_X,
            0x1800,
            virtual_address,
            1,
            ELF64_PAGE_SIZE,
            1
        );
    }

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_LIMIT_EXCEEDED
    );
}


/*
 * Test 26:
 * A short ELF header is rejected.
 */
static bool test_short_elf_header(void) {

    test_build_valid_image();

    return test_expect_status(
        test_buffer,
        TEST_ELF_HEADER_SIZE - 1U,
        ELF64_STATUS_TRUNCATED
    );
}

/*
 * Verify that parser output cannot overlap its input file.
 *
 * The parser must reject this before changing any ELF bytes.
 */
static bool test_parse_output_alias_rejected(void) {

    test_build_valid_image();

    uint64_t checksum_before = 1469598103934665603ULL;

    for (uint64_t i = 0;
         i < TEST_ELF_BUFFER_SIZE;
         i++) {

        checksum_before ^= test_buffer[i];
        checksum_before *= 1099511628211ULL;
    }

    elf64_status_t status =
        elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            (elf64_image_t *)(void *)test_buffer
        );

    uint64_t checksum_after = 1469598103934665603ULL;

    for (uint64_t i = 0;
         i < TEST_ELF_BUFFER_SIZE;
         i++) {

        checksum_after ^= test_buffer[i];
        checksum_after *= 1099511628211ULL;
    }

    return status == ELF64_STATUS_INVALID_ARGUMENT &&
        checksum_before == checksum_after;
}


/*
 * A file offset close to UINT64_MAX must not wrap when
 * the file-backed byte range is validated.
 */
static bool test_segment_file_offset_overflow(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        UINT64_MAX - 1ULL,
        0x400000,
        4,
        0x1000,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_TRUNCATED
    );
}


/*
 * A virtual range starting near UINT64_MAX must be rejected.
 *
 * The parser must not allow address arithmetic to wrap.
 */
static bool test_segment_virtual_address_overflow(void) {

    test_build_valid_image();

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        UINT64_MAX - 0x7FFULL,
        4,
        0x1000,
        1
    );

    return test_expect_status(
        test_buffer,
        sizeof(test_buffer),
        ELF64_STATUS_INVALID_SEGMENT
    );
}


/*
 * A zero-sized PT_LOAD must not contribute a mapped segment.
 *
 * The executable still contains one valid nonempty
 * executable segment.
 */
static bool test_zero_length_load_segment_ignored(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        2
    );

    /*
     * Empty segment.
     */
    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x500000,
        0,
        0,
        1
    );

    /*
     * Real executable segment.
     */
    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        4,
        ELF64_PAGE_SIZE,
        ELF64_PAGE_SIZE
    );

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    return test_output.program_header_count == 2 &&
        test_output.load_segment_count == 1 &&
        test_output.segments[0].virtual_address ==
            0x400000ULL;
}

/*
 * Release prepared frames and verify that PMM's free-frame
 * count has returned to its original value.
 */
static bool test_release_prepared_and_restore(
    uint64_t *page_count,
    uint64_t original_free_frames
) {
    if (page_count == NULL) {
        return false;
    }

    if (*page_count != 0) {

        if (elf64_release_prepared_pages(
                test_prepared_pages,
                page_count
            ) != ELF64_STATUS_OK) {

            return false;
        }
    }

    if (*page_count != 0) {
        return false;
    }

    return pmm_get_free_frames() ==
        original_free_frames;
}

static bool test_page_plan_basic(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t page_count = UINT64_MAX;

    if (elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            &page_count
        ) != ELF64_STATUS_OK) {

        return false;
    }

    return page_count == 1 &&
        test_plan_pages[0].virtual_address == 0x400000 &&
        test_plan_pages[0].segment_flags ==
            (TEST_ELF_PF_R | TEST_ELF_PF_X) &&
        test_plan_pages[0].reserved == 0;
}

static bool test_page_plan_shared_page_deduplication(void) {

    test_build_valid_image();

    test_write_u16(&test_buffer[56], 2);

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        0x100,
        0x100,
        1
    );

    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1200,
        0x400100,
        0x100,
        0x100,
        1
    );

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t page_count = UINT64_MAX;

    if (elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            &page_count
        ) != ELF64_STATUS_OK) {

        return false;
    }

    return page_count == 1 &&
        test_plan_pages[0].virtual_address == 0x400000;
}

static bool test_page_plan_sorted_order(void) {

    test_build_valid_image();

    test_write_u64(
        &test_buffer[24],
        0x402000
    );

    test_write_u16(
        &test_buffer[56],
        2
    );

    /*
     * First program header: higher executable page.
     */
    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x402000,
        1,
        0x1000,
        1
    );

    /*
     * Second program header: lower writable page.
     */
    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_W,
        0x1001,
        0x400000,
        1,
        0x1000,
        1
    );

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t page_count = UINT64_MAX;

    if (elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            &page_count
        ) != ELF64_STATUS_OK) {

        return false;
    }

    return page_count == 2 &&
        test_plan_pages[0].virtual_address == 0x400000 &&
        test_plan_pages[0].segment_flags ==
            (TEST_ELF_PF_R | TEST_ELF_PF_W) &&
        test_plan_pages[1].virtual_address == 0x402000 &&
        test_plan_pages[1].segment_flags ==
            (TEST_ELF_PF_R | TEST_ELF_PF_X);
}

static bool test_page_plan_capacity_failure(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    test_plan_pages[0].virtual_address =
        0x1122334455667788ULL;

    test_plan_pages[0].segment_flags = 0xAABBCCDDU;
    test_plan_pages[0].reserved = 0x12345678U;

    uint64_t page_count =
        0x8877665544332211ULL;

    elf64_status_t status =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            0,
            &page_count
        );

    return status ==
            ELF64_STATUS_INSUFFICIENT_CAPACITY &&
        page_count == 0x8877665544332211ULL &&
        test_plan_pages[0].virtual_address ==
            0x1122334455667788ULL &&
        test_plan_pages[0].segment_flags ==
            0xAABBCCDDU &&
        test_plan_pages[0].reserved ==
            0x12345678U;
}

/*
 * Verify that page-plan capacity is checked exactly.
 *
 * Two required pages:
 *     Capacity 1: rejected, output unchanged.
 *     Capacity 2: accepted, both pages correctly ordered.
 */
static bool test_page_plan_exact_capacity_boundary(void) {

    test_build_valid_image();

    test_write_u64(
        &test_buffer[24],
        0x402000
    );

    test_write_u16(
        &test_buffer[56],
        2
    );

    /*
     * First program header: higher executable page.
     */
    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x402000,
        1,
        ELF64_PAGE_SIZE,
        1
    );

    /*
     * Second program header: lower writable page.
     */
    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_W,
        0x1001,
        0x400000,
        1,
        ELF64_PAGE_SIZE,
        1
    );

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    test_plan_pages[0].virtual_address =
        0x1111222233334444ULL;

    test_plan_pages[0].segment_flags =
        0xAABBCCDDU;

    test_plan_pages[0].reserved =
        0x12345678U;

    test_plan_pages[1].virtual_address =
        0x5555666677778888ULL;

    test_plan_pages[1].segment_flags =
        0x11223344U;

    test_plan_pages[1].reserved =
        0x87654321U;

    elf64_page_plan_entry_t before[2] = {
        test_plan_pages[0],
        test_plan_pages[1]
    };

    const uint64_t count_sentinel =
        0x8877665544332211ULL;

    uint64_t page_count = count_sentinel;

    /*
     * One entry cannot hold the two-page plan.
     */
    elf64_status_t insufficient =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            1,
            &page_count
        );

    bool unchanged =
        page_count == count_sentinel &&
        test_plan_pages[0].virtual_address ==
            before[0].virtual_address &&
        test_plan_pages[0].segment_flags ==
            before[0].segment_flags &&
        test_plan_pages[0].reserved ==
            before[0].reserved &&
        test_plan_pages[1].virtual_address ==
            before[1].virtual_address &&
        test_plan_pages[1].segment_flags ==
            before[1].segment_flags &&
        test_plan_pages[1].reserved ==
            before[1].reserved;

    if (insufficient != ELF64_STATUS_INSUFFICIENT_CAPACITY ||
        !unchanged) {

        return false;
    }

    /*
     * Exactly two entries must be sufficient.
     */
    page_count = UINT64_MAX;

    elf64_status_t exact =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            2,
            &page_count
        );

    return exact == ELF64_STATUS_OK &&
        page_count == 2 &&
        test_plan_pages[0].virtual_address == 0x400000ULL &&
        test_plan_pages[0].segment_flags ==
            (TEST_ELF_PF_R | TEST_ELF_PF_W) &&
        test_plan_pages[1].virtual_address == 0x402000ULL &&
        test_plan_pages[1].segment_flags ==
            (TEST_ELF_PF_R | TEST_ELF_PF_X);
}

/*
 * Test that page-plan input and output ranges cannot overlap.
 *
 * This exercises:
 *     - Output array overlapping the parsed image.
 *     - Output array overlapping the ELF buffer.
 *     - Count pointer overlapping the parsed image.
 *     - Count pointer overlapping the ELF buffer.
 *     - Count pointer overlapping the output array.
 */
static bool test_page_plan_output_alias_rejected(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    const uint64_t sentinel =
        0x8877665544332211ULL;

    uint64_t page_count = sentinel;

    /*
     * Case 1: output array overlaps the parsed image.
     */
    elf64_status_t status =
        elf64_build_page_plan(
            &test_output,
            (elf64_page_plan_entry_t *)(void *)&test_output,
            8,
            &page_count
        );

    if (status != ELF64_STATUS_INVALID_ARGUMENT ||
        page_count != sentinel) {

        return false;
    }

    /*
     * Case 2: output array overlaps the ELF file.
     */
    status =
        elf64_build_page_plan(
            &test_output,
            (elf64_page_plan_entry_t *)(void *)test_buffer,
            8,
            &page_count
        );

    if (status != ELF64_STATUS_INVALID_ARGUMENT ||
        page_count != sentinel) {

        return false;
    }

    /*
     * Case 3: count pointer overlaps the parsed image.
     *
     * file_size remains valid until the function rejects
     * the overlap.
     */
    uint64_t original_file_size =
        test_output.file_size;

    status =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            &test_output.file_size
        );

    if (status != ELF64_STATUS_INVALID_ARGUMENT ||
        test_output.file_size != original_file_size) {

        return false;
    }

    /*
     * Case 4: count pointer overlaps the ELF file.
     */
    uint64_t original_file_byte =
        *(uint64_t *)(void *)&test_buffer[0x3000];

    status =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            (uint64_t *)(void *)&test_buffer[0x3000]
        );

    if (status != ELF64_STATUS_INVALID_ARGUMENT ||
        *(uint64_t *)(void *)&test_buffer[0x3000] !=
            original_file_byte) {

        return false;
    }

    /*
     * Case 5: count pointer overlaps the output array.
     */
    test_plan_pages[0].virtual_address = sentinel;

    status =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            &test_plan_pages[0].virtual_address
        );

    return status == ELF64_STATUS_INVALID_ARGUMENT &&
        test_plan_pages[0].virtual_address == sentinel;
}

static bool test_page_plan_rejects_modified_image(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    /*
     * Corrupt a cached segment boundary after parsing.
     */
    test_output.segments[0].memory_end++;

    test_plan_pages[0].virtual_address =
        0x1122334455667788ULL;

    uint64_t page_count = 0x8877665544332211ULL;

    elf64_status_t status =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            &page_count
        );

    return status == ELF64_STATUS_INVALID_SEGMENT &&
        page_count == 0x8877665544332211ULL &&
        test_plan_pages[0].virtual_address ==
            0x1122334455667788ULL;
}

/*
 * Apply one deliberate mutation to a previously valid image
 * and verify that page-plan construction rejects it safely.
 */
static bool test_page_plan_mutation_case(
    uint32_t mutation,
    elf64_status_t expected_status
) {
    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    switch (mutation) {

        case 0:
            /*
             * Invalid file-backed range.
             */
            test_output.segments[0].file_offset =
                UINT64_MAX;
            break;

        case 1:
            /*
             * Illegal writable + executable permissions.
             */
            test_output.segments[0].flags =
                ELF64_PF_R |
                ELF64_PF_W |
                ELF64_PF_X;
            break;

        case 2:
            /*
             * Cached page range no longer matches the segment.
             */
            test_output.segments[0].page_start +=
                ELF64_PAGE_SIZE;
            break;

        case 3:
            /*
             * Entry point now falls outside executable memory.
             */
            test_output.entry = 0x402000ULL;
            break;

        case 4:
            /*
             * No loadable segments.
             */
            test_output.load_segment_count = 0;
            break;

        case 5:
            /*
             * Invalid alignment.
             */
            test_output.segments[0].alignment = 3;
            break;

        default:
            return false;
    }

    test_plan_pages[0].virtual_address =
        0x1122334455667788ULL;

    test_plan_pages[0].segment_flags =
        0xAABBCCDDU;

    test_plan_pages[0].reserved =
        0x12345678U;

    const uint64_t count_sentinel =
        0x8877665544332211ULL;

    uint64_t page_count = count_sentinel;

    uint64_t original_free_frames =
        pmm_get_free_frames();

    elf64_status_t status =
        elf64_build_page_plan(
            &test_output,
            test_plan_pages,
            8,
            &page_count
        );

    return status == expected_status &&
        page_count == count_sentinel &&
        test_plan_pages[0].virtual_address ==
            0x1122334455667788ULL &&
        test_plan_pages[0].segment_flags ==
            0xAABBCCDDU &&
        test_plan_pages[0].reserved ==
            0x12345678U &&
        pmm_get_free_frames() == original_free_frames;
}


/*
 * Exercise several independent parsed-image corruption cases.
 */
static bool test_page_plan_mutation_matrix(void) {

    return
        test_page_plan_mutation_case(
            0,
            ELF64_STATUS_TRUNCATED
        ) &&
        test_page_plan_mutation_case(
            1,
            ELF64_STATUS_PERMISSION_CONFLICT
        ) &&
        test_page_plan_mutation_case(
            2,
            ELF64_STATUS_INVALID_SEGMENT
        ) &&
        test_page_plan_mutation_case(
            3,
            ELF64_STATUS_INVALID_ENTRY
        ) &&
        test_page_plan_mutation_case(
            4,
            ELF64_STATUS_INVALID_FORMAT
        ) &&
        test_page_plan_mutation_case(
            5,
            ELF64_STATUS_INVALID_SEGMENT
        );
}

/*
 * Verify that prepared-page storage and the output count
 * cannot overwrite the parsed image, ELF buffer, or each other.
 *
 * These failures must occur before physical allocation.
 */
static bool test_prepare_output_alias_rejected(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t original_free_frames =
        pmm_get_free_frames();

    const uint64_t sentinel =
        0xAABBCCDDEEFF0011ULL;

    uint64_t page_count = sentinel;

    /*
     * Case 1: prepared-page storage overlaps ELF bytes.
     */
    elf64_status_t status =
        elf64_prepare_pages(
            &test_output,
            (elf64_prepared_page_t *)(void *)test_buffer,
            8,
            &page_count
        );

    if (status != ELF64_STATUS_INVALID_ARGUMENT ||
        page_count != sentinel) {

        return false;
    }

    /*
     * Case 2: prepared-page storage overlaps the image object.
     */
    status =
        elf64_prepare_pages(
            &test_output,
            (elf64_prepared_page_t *)(void *)&test_output,
            8,
            &page_count
        );

    if (status != ELF64_STATUS_INVALID_ARGUMENT ||
        page_count != sentinel) {

        return false;
    }

    /*
     * Case 3: count pointer overlaps the image object.
     */
    uint64_t original_file_size =
        test_output.file_size;

    status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            8,
            &test_output.file_size
        );

    if (status != ELF64_STATUS_INVALID_ARGUMENT ||
        test_output.file_size != original_file_size) {

        return false;
    }

    /*
     * Case 4: count pointer overlaps prepared-page storage.
     */
    test_prepared_pages[0].virtual_address = sentinel;

    status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            8,
            &test_prepared_pages[0].virtual_address
        );

    return status == ELF64_STATUS_INVALID_ARGUMENT &&
        test_prepared_pages[0].virtual_address == sentinel &&
        pmm_get_free_frames() == original_free_frames;
}

/*
 * Duplicate physical-frame records must be rejected before
 * any PMM frame is released.
 */
static bool test_release_duplicate_frame_rejected_atomically(void) {

    uint64_t original_free_frames =
        pmm_get_free_frames();

    uint64_t physical =
        pmm_alloc_frame();

    if (physical == 0) {
        return false;
    }

    test_prepared_pages[0].virtual_address =
        0x400000ULL;

    test_prepared_pages[0].physical_address =
        physical;

    test_prepared_pages[0].segment_flags =
        ELF64_PF_R | ELF64_PF_X;

    test_prepared_pages[0].reserved = 0;

    test_prepared_pages[1].virtual_address =
        0x401000ULL;

    /*
     * Deliberately claim the exact same physical frame twice.
     */
    test_prepared_pages[1].physical_address =
        physical;

    test_prepared_pages[1].segment_flags =
        ELF64_PF_R | ELF64_PF_X;

    test_prepared_pages[1].reserved = 0;

    uint64_t page_count = 2;

    elf64_status_t status =
        elf64_release_prepared_pages(
            test_prepared_pages,
            &page_count
        );

    bool rejected_atomically =
        status == ELF64_STATUS_INVALID_ARGUMENT &&
        page_count == 2 &&
        pmm_frame_to_virt(physical) != NULL &&
        pmm_get_free_frames() ==
            original_free_frames - 1ULL;

    /*
     * The frame is still ours because duplicate validation
     * must happen before the first release.
     */
    bool manually_freed =
        pmm_free_frame(physical);

    return rejected_atomically &&
        manually_freed &&
        pmm_get_free_frames() == original_free_frames;
}

/*
 * Inject an exact physical-frame release failure.
 *
 * The successful release of the unrelated frame must still
 * make progress. The failed record must remain available
 * for a later retry.
 */
static bool test_release_frame_failure_retry(void) {

    uint64_t original_free_frames =
        pmm_get_free_frames();

    uint64_t first_frame =
        pmm_alloc_frame();

    uint64_t second_frame =
        pmm_alloc_frame();

    if (first_frame == 0 ||
        second_frame == 0 ||
        first_frame == second_frame) {

        if (first_frame != 0) {
            (void)pmm_free_frame(first_frame);
        }

        if (second_frame != 0) {
            (void)pmm_free_frame(second_frame);
        }

        return false;
    }

    test_prepared_pages[0].virtual_address =
        0x400000ULL;

    test_prepared_pages[0].physical_address =
        first_frame;

    test_prepared_pages[0].segment_flags =
        ELF64_PF_R | ELF64_PF_X;

    test_prepared_pages[0].reserved = 0;

    test_prepared_pages[1].virtual_address =
        0x401000ULL;

    test_prepared_pages[1].physical_address =
        second_frame;

    test_prepared_pages[1].segment_flags =
        ELF64_PF_R | ELF64_PF_W;

    test_prepared_pages[1].reserved = 0;

    uint64_t page_count = 2;

    bool passed = false;

    if (!pmm_test_fail_free_frame_once(first_frame)) {
        goto cleanup;
    }

    elf64_status_t status =
        elf64_release_prepared_pages(
            test_prepared_pages,
            &page_count
        );

    /*
     * The failed release must be the only remaining record.
     */
    bool retained_correctly =
        status == ELF64_STATUS_CLEANUP_FAILED &&
        page_count == 1 &&
        test_prepared_pages[0].physical_address ==
            first_frame &&
        pmm_frame_to_virt(first_frame) != NULL &&
        pmm_frame_to_virt(second_frame) == NULL &&
        pmm_get_free_frames() ==
            original_free_frames - 1ULL;

    if (!retained_correctly) {
        goto cleanup;
    }

    /*
     * Clear the one-shot failure and retry.
     */
    pmm_test_clear_free_frame_failure();

    status =
        elf64_release_prepared_pages(
            test_prepared_pages,
            &page_count
        );

    passed =
        status == ELF64_STATUS_OK &&
        page_count == 0 &&
        pmm_frame_to_virt(first_frame) == NULL &&
        pmm_frame_to_virt(second_frame) == NULL &&
        pmm_get_free_frames() == original_free_frames;

cleanup:

    pmm_test_clear_free_frame_failure();

    /*
     * Ensure no frames leak if an assertion fails.
     */
    if (page_count != 0) {
        (void)elf64_release_prepared_pages(
            test_prepared_pages,
            &page_count
        );
    }

    if (pmm_frame_to_virt(first_frame) != NULL) {
        (void)pmm_free_frame(first_frame);
    }

    if (pmm_frame_to_virt(second_frame) != NULL) {
        (void)pmm_free_frame(second_frame);
    }

    return passed &&
        pmm_get_free_frames() == original_free_frames;
}

/*
 * Test:
 *
 * Construct an executable requiring three physical pages.
 *
 * Inject allocation failure after:
 *     0 successful allocations.
 *     1 successful allocation.
 *     2 successful allocations.
 *
 * Normal rollback must restore the PMM free-frame count.
 *
 * Then combine:
 *     - Allocation failure after two successful allocations.
 *     - A physical-frame release failure during rollback.
 *
 * The preparation routine must return CLEANUP_FAILED and
 * preserve the remaining physical-frame record for retry.
 */
static bool test_prepare_allocation_failure_rollback(void) {

    test_build_valid_image();

    /*
     * Expand the existing segment to three pages.
     *
     * Its four file-backed bytes remain unchanged.
     */
    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        4,
        3ULL * ELF64_PAGE_SIZE,
        ELF64_PAGE_SIZE
    );

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t original_free_frames =
        pmm_get_free_frames();

    /*
     * PART 1:
     *
     * Normal allocation failure after 0, 1, or 2 successful
     * allocations. All previously allocated frames must be
     * released by preparation rollback.
     */
    for (uint64_t successful_allocations = 0;
         successful_allocations < 3;
         successful_allocations++) {

        pmm_test_clear_alloc_failure();
        pmm_test_clear_free_after_failure();
        pmm_test_clear_free_frame_failure();

        uint64_t page_count =
            0xAABBCCDDEEFF0011ULL;

        if (!pmm_test_fail_alloc_after(
                successful_allocations
            )) {

            return false;
        }

        elf64_status_t status =
            elf64_prepare_pages(
                &test_output,
                test_prepared_pages,
                8,
                &page_count
            );

        pmm_test_clear_alloc_failure();

        if (status != ELF64_STATUS_OUT_OF_MEMORY ||
            page_count != 0xAABBCCDDEEFF0011ULL ||
            pmm_get_free_frames() !=
                original_free_frames) {

            pmm_test_clear_free_after_failure();
            pmm_test_clear_free_frame_failure();

            return false;
        }
    }

    /*
     * PART 2:
     *
     * Allocate two frames, fail the third allocation, and
     * simultaneously fail one physical-frame release.
     *
     * The preparation routine must retain the one frame
     * that PMM could not release.
     */
    pmm_test_clear_alloc_failure();
    pmm_test_clear_free_after_failure();
    pmm_test_clear_free_frame_failure();

    uint64_t page_count = 0x1122334455667788ULL;

    if (!pmm_test_fail_alloc_after(2)) {
        return false;
    }

    if (!pmm_test_fail_free_after(0)) {
        pmm_test_clear_alloc_failure();
        return false;
    }

    elf64_status_t status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            8,
            &page_count
        );

    /*
     * The failure hooks are one-shot or explicitly cleared.
     */
    pmm_test_clear_alloc_failure();
    pmm_test_clear_free_after_failure();
    pmm_test_clear_free_frame_failure();

    bool retained_one_frame =
        status == ELF64_STATUS_CLEANUP_FAILED &&
        page_count == 1 &&
        test_prepared_pages[0].physical_address != 0 &&
        test_prepared_pages[0].virtual_address >=
            0x400000ULL &&
        test_prepared_pages[0].virtual_address <
            0x403000ULL &&
        pmm_frame_to_virt(
            test_prepared_pages[0].physical_address
        ) != NULL &&
        pmm_get_free_frames() ==
            original_free_frames - 1ULL;

    if (!retained_one_frame) {

        /*
         * page_count may contain a retained frame even
         * if an assertion above fails.
         */
        if (page_count <= ELF64_MAX_IMAGE_PAGES &&
            page_count != 0) {

            (void)elf64_release_prepared_pages(
                test_prepared_pages,
                &page_count
            );
        }

        return false;
    }

    /*
     * PART 3:
     *
     * Retry the exact retained record after removing
     * all injected failures.
     */
    status =
        elf64_release_prepared_pages(
            test_prepared_pages,
            &page_count
        );

    return status == ELF64_STATUS_OK &&
        page_count == 0 &&
        pmm_get_free_frames() == original_free_frames;
}

static bool test_unique_shared_page_limit_accounting(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        ELF64_MAX_LOAD_SEGMENTS
    );

    uint64_t cursor = 0x400000ULL;

    for (uint16_t i = 0;
         i < ELF64_MAX_LOAD_SEGMENTS;
         i++) {

        uint64_t memory_size;

        if (i == 0) {

            memory_size =
                512ULL * ELF64_PAGE_SIZE + 0x800ULL;

        } else if (
            i == ELF64_MAX_LOAD_SEGMENTS - 1
        ) {

            memory_size =
                512ULL * ELF64_PAGE_SIZE - 0x800ULL;

        } else {

            memory_size =
                512ULL * ELF64_PAGE_SIZE;
        }

        test_write_program_header(
            test_buffer,
            i,
            TEST_ELF_PT_LOAD,
            TEST_ELF_PF_R | TEST_ELF_PF_X,
            0x1000ULL + i,
            cursor,
            1,
            memory_size,
            1
        );

        cursor += memory_size;
    }

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    return test_output.load_segment_count ==
        ELF64_MAX_LOAD_SEGMENTS;
}

/*
 * Test 33:
 * A basic executable is prepared correctly.
 *
 * Its four file-backed bytes must be copied.
 * The remaining bytes in the page must be zero.
 */
static bool test_prepare_basic_and_bss(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t original_free_frames =
        pmm_get_free_frames();

    uint64_t page_count = 0;

    elf64_status_t status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            8,
            &page_count
        );

    bool valid =
        status == ELF64_STATUS_OK &&
        page_count == 1;

    if (valid) {

        const elf64_prepared_page_t *page =
            &test_prepared_pages[0];

        if (page->virtual_address != 0x400000 ||
            page->physical_address == 0 ||
            (page->physical_address &
                (PMM_PAGE_SIZE - 1ULL)) != 0 ||
            page->segment_flags !=
                (TEST_ELF_PF_R | TEST_ELF_PF_X) ||
            page->reserved != 0) {

            valid = false;
        }
    }

    if (valid) {

        uint8_t *physical_page =
            (uint8_t *)pmm_frame_to_virt(
                test_prepared_pages[0].physical_address
            );

        if (physical_page == NULL) {

            valid = false;

        } else {

            const uint8_t expected[4] = {
                0x90, 0x90, 0x90, 0xC3
            };

            for (uint64_t i = 0; i < 4; i++) {

                if (physical_page[i] != expected[i]) {
                    valid = false;
                    break;
                }
            }

            /*
             * The rest of the frame must be zero.
             */
            for (uint64_t i = 4;
                 valid && i < PMM_PAGE_SIZE;
                 i++) {

                if (physical_page[i] != 0) {
                    valid = false;
                }
            }
        }
    }

    bool cleanup_ok =
        test_release_prepared_and_restore(
            &page_count,
            original_free_frames
        );

    return valid && cleanup_ok;
}

/*
 * Test 34:
 * A file-backed segment crosses a 4 KiB boundary.
 *
 * Its first 16 bytes belong at the end of page 0.
 * Its next 16 bytes belong at the beginning of page 1.
 *
 * All remaining bytes must remain zero.
 */
static bool test_prepare_cross_page_segment(void) {

    test_build_valid_image();

    const uint64_t virtual_start = 0x400FF0ULL;
    const uint64_t file_start = 0x1FF0ULL;

    test_write_u64(
        &test_buffer[24],
        virtual_start
    );

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        file_start,
        virtual_start,
        32,
        0x1010,
        1
    );

    /*
     * Create a recognizable 32-byte source pattern.
     */
    for (uint64_t i = 0; i < 32; i++) {

        test_buffer[file_start + i] =
            (uint8_t)(0xA0U + i);
    }

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t original_free_frames =
        pmm_get_free_frames();

    uint64_t page_count = 0;

    elf64_status_t status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            8,
            &page_count
        );

    bool valid =
        status == ELF64_STATUS_OK &&
        page_count == 2;

    uint8_t *first_page = NULL;
    uint8_t *second_page = NULL;

    if (valid) {

        valid =
            test_prepared_pages[0].virtual_address ==
                0x400000ULL &&
            test_prepared_pages[1].virtual_address ==
                0x401000ULL;
    }

    if (valid) {

        first_page =
            (uint8_t *)pmm_frame_to_virt(
                test_prepared_pages[0].physical_address
            );

        second_page =
            (uint8_t *)pmm_frame_to_virt(
                test_prepared_pages[1].physical_address
            );

        if (first_page == NULL ||
            second_page == NULL) {

            valid = false;
        }
    }

    if (valid) {

        /*
         * First page:
         * 0x000 through 0xFEF must be zero.
         * 0xFF0 through 0xFFF contain the first 16 bytes.
         */
        for (uint64_t i = 0; i < PMM_PAGE_SIZE; i++) {

            uint8_t expected = 0;

            if (i >= 0xFF0ULL) {

                expected =
                    (uint8_t)(0xA0U + (i - 0xFF0ULL));
            }

            if (first_page[i] != expected) {
                valid = false;
                break;
            }
        }
    }

    if (valid) {

        /*
         * Second page:
         * The first 16 bytes contain the remaining file data.
         * Everything after them must be zero.
         */
        for (uint64_t i = 0; i < PMM_PAGE_SIZE; i++) {

            uint8_t expected = 0;

            if (i < 16) {
                expected = (uint8_t)(0xB0U + i);
            }

            if (second_page[i] != expected) {
                valid = false;
                break;
            }
        }
    }

    bool cleanup_ok =
        test_release_prepared_and_restore(
            &page_count,
            original_free_frames
        );

    return valid && cleanup_ok;
}

/*
 * Test 35:
 * Two compatible, non-overlapping segments share one page.
 *
 * Both segments' data must be present in the same allocated
 * physical frame, and the gap after them must be zero.
 */
static bool test_prepare_shared_page(void) {

    test_build_valid_image();

    test_write_u16(
        &test_buffer[56],
        2
    );

    test_write_program_header(
        test_buffer,
        0,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1000,
        0x400000,
        0x100,
        0x100,
        1
    );

    test_write_program_header(
        test_buffer,
        1,
        TEST_ELF_PT_LOAD,
        TEST_ELF_PF_R | TEST_ELF_PF_X,
        0x1200,
        0x400100,
        0x100,
        0x100,
        1
    );

    for (uint64_t i = 0; i < 0x100; i++) {

        test_buffer[0x1000 + i] = 0x31;
        test_buffer[0x1200 + i] = 0x72;
    }

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    uint64_t original_free_frames =
        pmm_get_free_frames();

    uint64_t page_count = 0;

    elf64_status_t status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            8,
            &page_count
        );

    bool valid =
        status == ELF64_STATUS_OK &&
        page_count == 1;

    if (valid &&
        test_prepared_pages[0].virtual_address !=
            0x400000ULL) {

        valid = false;
    }

    if (valid) {

        uint8_t *physical_page =
            (uint8_t *)pmm_frame_to_virt(
                test_prepared_pages[0].physical_address
            );

        if (physical_page == NULL) {

            valid = false;

        } else {

            for (uint64_t i = 0; i < PMM_PAGE_SIZE; i++) {

                uint8_t expected = 0;

                if (i < 0x100) {
                    expected = 0x31;

                } else if (i < 0x200) {
                    expected = 0x72;
                }

                if (physical_page[i] != expected) {
                    valid = false;
                    break;
                }
            }
        }
    }

    bool cleanup_ok =
        test_release_prepared_and_restore(
            &page_count,
            original_free_frames
        );

    return valid && cleanup_ok;
}

/*
 * Test 36:
 * Insufficient output capacity is detected before allocation.
 *
 * Neither the output records nor the output count may change.
 */
static bool test_prepare_insufficient_capacity(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    const uint64_t sentinel_count =
        0x8877665544332211ULL;

    test_prepared_pages[0].virtual_address =
        0x1122334455667788ULL;

    test_prepared_pages[0].physical_address =
        0x8877665544332211ULL;

    test_prepared_pages[0].segment_flags =
        0xAABBCCDDU;

    test_prepared_pages[0].reserved =
        0x12345678U;

    uint64_t page_count = sentinel_count;

    uint64_t original_free_frames =
        pmm_get_free_frames();

    elf64_status_t status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            0,
            &page_count
        );

    return status ==
            ELF64_STATUS_INSUFFICIENT_CAPACITY &&
        page_count == sentinel_count &&
        test_prepared_pages[0].virtual_address ==
            0x1122334455667788ULL &&
        test_prepared_pages[0].physical_address ==
            0x8877665544332211ULL &&
        test_prepared_pages[0].segment_flags ==
            0xAABBCCDDU &&
        test_prepared_pages[0].reserved ==
            0x12345678U &&
        pmm_get_free_frames() == original_free_frames;
}

/*
 * Test 37:
 * The parsed image is modified after parsing.
 *
 * Preparation must reject inconsistent cached metadata
 * without allocating frames or changing the output.
 */
static bool test_prepare_rejects_modified_image(void) {

    test_build_valid_image();

    if (elf64_parse(
            test_buffer,
            sizeof(test_buffer),
            &test_output
        ) != ELF64_STATUS_OK) {

        return false;
    }

    test_output.segments[0].memory_end++;

    test_prepared_pages[0].virtual_address =
        0x1122334455667788ULL;

    test_prepared_pages[0].physical_address =
        0x8877665544332211ULL;

    uint64_t sentinel_count =
        0xAABBCCDDEEFF0011ULL;

    uint64_t page_count = sentinel_count;

    uint64_t original_free_frames =
        pmm_get_free_frames();

    elf64_status_t status =
        elf64_prepare_pages(
            &test_output,
            test_prepared_pages,
            8,
            &page_count
        );

    return status == ELF64_STATUS_INVALID_SEGMENT &&
        page_count == sentinel_count &&
        test_prepared_pages[0].virtual_address ==
            0x1122334455667788ULL &&
        test_prepared_pages[0].physical_address ==
            0x8877665544332211ULL &&
        pmm_get_free_frames() == original_free_frames;
}

/*
 * Test 38:
 * Releasing an already empty prepared-page list is harmless.
 */
static bool test_release_empty_prepared_pages(void) {

    uint64_t page_count = 0;

    uint64_t original_free_frames =
        pmm_get_free_frames();

    elf64_status_t status =
        elf64_release_prepared_pages(
            NULL,
            &page_count
        );

    return status == ELF64_STATUS_OK &&
        page_count == 0 &&
        pmm_get_free_frames() == original_free_frames;
}

/*
 * Run all ELF64 parser tests.
 */
bool elf64_run_tests(void) {

    if (!test_valid_executable()) {
        return false;
    }

    if (!test_compatible_shared_page()) {
        return false;
    }

    if (!test_conflicting_shared_page()) {
        return false;
    }

    if (!test_overlapping_memory_ranges()) {
        return false;
    }

    if (!test_writable_executable_segment()) {
        return false;
    }

    if (!test_entry_outside_executable_segment()) {
        return false;
    }

    if (!test_entry_in_non_executable_segment()) {
        return false;
    }

    if (!test_segment_without_read_permission()) {
        return false;
    }

    if (!test_unknown_segment_flags()) {
        return false;
    }

    if (!test_truncated_segment_data()) {
        return false;
    }

    if (!test_file_size_exceeds_memory_size()) {
        return false;
    }

    if (!test_invalid_alignment()) {
        return false;
    }

    if (!test_alignment_congruence()) {
        return false;
    }

    if (!test_low_user_address()) {
        return false;
    }

    if (!test_user_address_limit()) {
        return false;
    }

    if (!test_dynamic_segment_rejected()) {
        return false;
    }

    if (!test_executable_stack_rejected()) {
        return false;
    }

    if (!test_dynamic_object_type_rejected()) {
        return false;
    }

    if (!test_wrong_elf_class()) {
        return false;
    }

    if (!test_wrong_byte_order()) {
        return false;
    }

    if (!test_truncated_program_header_table()) {
        return false;
    }

    if (!test_program_header_offset_out_of_bounds()) {
        return false;
    }

    if (!test_program_header_count_limit()) {
        return false;
    }

    if (!test_image_page_limit()) {
        return false;
    }

    if (!test_load_segment_count_limit()) {
        return false;
    }

    if (!test_short_elf_header()) {
        return false;
    }

    if (!test_page_plan_basic()) {
        return false;
    }

    if (!test_page_plan_shared_page_deduplication()) {
        return false;
    }

    if (!test_page_plan_sorted_order()) {
        return false;
    }

    if (!test_page_plan_capacity_failure()) {
        return false;
    }

    if (!test_page_plan_rejects_modified_image()) {
        return false;
    }

    if (!test_unique_shared_page_limit_accounting()) {
        return false;
    }

    if (!test_prepare_basic_and_bss()) {
        return false;
    }

    if (!test_prepare_cross_page_segment()) {
        return false;
    }

    if (!test_prepare_shared_page()) {
        return false;
    }

    if (!test_prepare_insufficient_capacity()) {
        return false;
    }

    if (!test_prepare_rejects_modified_image()) {
        return false;
    }

    if (!test_release_empty_prepared_pages()) {
        return false;
    }

    /*
     * Additional parser and output-integrity tests.
     */
    if (!test_parse_output_alias_rejected()) {
        return false;
    }

    if (!test_segment_file_offset_overflow()) {
        return false;
    }

    if (!test_segment_virtual_address_overflow()) {
        return false;
    }

    if (!test_zero_length_load_segment_ignored()) {
        return false;
    }

    /*
     * Page-plan boundaries and alias protection.
     */
    if (!test_page_plan_exact_capacity_boundary()) {
        return false;
    }

    if (!test_page_plan_output_alias_rejected()) {
        return false;
    }

    if (!test_page_plan_mutation_matrix()) {
        return false;
    }

    /*
     * Preparation output-integrity tests.
     */
    if (!test_prepare_output_alias_rejected()) {
        return false;
    }

    /*
     * Prepared-frame ownership and release tests.
     */
    if (!test_release_duplicate_frame_rejected_atomically()) {
        return false;
    }

    if (!test_release_frame_failure_retry()) {
        return false;
    }

    /*
     * Allocation failure and preparation rollback.
     */
    if (!test_prepare_allocation_failure_rollback()) {
        return false;
    }

    return true;
}
