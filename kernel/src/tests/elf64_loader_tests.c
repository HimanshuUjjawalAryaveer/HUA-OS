#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "exec/elf64.h"
#include "exec/elf64_loader.h"
#include "process/process.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "drivers/console/console.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "syscall/syscall_abi.h"


#define LOADER_TEST_BUFFER_SIZE  0x4000U

#define LOADER_TEST_HEADER_SIZE  64U
#define LOADER_TEST_PH_SIZE      56U
#define LOADER_TEST_PH_OFFSET    64U

#define LOADER_TEST_ET_EXEC      2U
#define LOADER_TEST_ET_DYN       3U
#define LOADER_TEST_EM_X86_64    62U

#define LOADER_TEST_PT_LOAD      1U

#define LOADER_TEST_CODE_VA      0x00400000ULL
#define LOADER_TEST_GAP_VA       0x00401000ULL
#define LOADER_TEST_DATA_VA      0x00402000ULL

#define LOADER_TEST_ENTRY        LOADER_TEST_CODE_VA

#define LOADER_TEST_BRANCH_DATA_VA      0x00600000ULL
#define LOADER_TEST_BRANCH_READONLY_VA  0x00800000ULL

#define LOADER_LAUNCH_SUCCESS_EXIT_CODE  0x12345678LL
#define LOADER_LAUNCH_FAILURE_EXIT_CODE  0xBADLL


static uint8_t loader_test_buffer[
    LOADER_TEST_BUFFER_SIZE
];


/*
 * Write a little-endian 16-bit integer.
 */
static void loader_test_write_u16(
    uint8_t *destination,
    uint16_t value
) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}


/*
 * Write a little-endian 32-bit integer.
 */
static void loader_test_write_u32(
    uint8_t *destination,
    uint32_t value
) {
    for (uint32_t i = 0; i < 4U; i++) {
        destination[i] =
            (uint8_t)(value >> (i * 8U));
    }
}


/*
 * Write a little-endian 64-bit integer.
 */
static void loader_test_write_u64(
    uint8_t *destination,
    uint64_t value
) {
    for (uint32_t i = 0; i < 8U; i++) {
        destination[i] =
            (uint8_t)(value >> (i * 8U));
    }
}


/*
 * Construct one PT_LOAD program header.
 */
static void loader_test_write_program_header(
    uint32_t index,
    uint32_t flags,
    uint64_t file_offset,
    uint64_t virtual_address,
    uint64_t file_size,
    uint64_t memory_size
) {
    uint64_t offset =
        LOADER_TEST_PH_OFFSET +
        (uint64_t)index * LOADER_TEST_PH_SIZE;

    uint8_t *header =
        &loader_test_buffer[offset];

    loader_test_write_u32(
        &header[0],
        LOADER_TEST_PT_LOAD
    );

    loader_test_write_u32(
        &header[4],
        flags
    );

    loader_test_write_u64(
        &header[8],
        file_offset
    );

    loader_test_write_u64(
        &header[16],
        virtual_address
    );

    loader_test_write_u64(
        &header[24],
        0
    );

    loader_test_write_u64(
        &header[32],
        file_size
    );

    loader_test_write_u64(
        &header[40],
        memory_size
    );

    loader_test_write_u64(
        &header[48],
        ELF64_PAGE_SIZE
    );
}


/*
 * Construct a valid two-segment ELF64 executable.
 *
 * Code:
 *     VA       0x00400000
 *     Flags    R | X
 *     File     4 bytes
 *     Memory   1 page
 *
 * Data:
 *     VA       0x00402000
 *     Flags    R | W
 *     File     4 bytes
 *     Memory   1 page
 *
 * The page at 0x00401000 must remain unmapped.
 */
static void loader_test_build_valid_image(void) {

    for (uint32_t i = 0;
         i < LOADER_TEST_BUFFER_SIZE;
         i++) {

        loader_test_buffer[i] = 0;
    }

    /*
     * ELF identification.
     */
    loader_test_buffer[0] = 0x7F;
    loader_test_buffer[1] = 'E';
    loader_test_buffer[2] = 'L';
    loader_test_buffer[3] = 'F';

    loader_test_buffer[4] = 2;
    loader_test_buffer[5] = 1;
    loader_test_buffer[6] = 1;

    /*
     * ELF header.
     */
    loader_test_write_u16(
        &loader_test_buffer[16],
        LOADER_TEST_ET_EXEC
    );

    loader_test_write_u16(
        &loader_test_buffer[18],
        LOADER_TEST_EM_X86_64
    );

    loader_test_write_u32(
        &loader_test_buffer[20],
        1
    );

    loader_test_write_u64(
        &loader_test_buffer[24],
        LOADER_TEST_ENTRY
    );

    loader_test_write_u64(
        &loader_test_buffer[32],
        LOADER_TEST_PH_OFFSET
    );

    loader_test_write_u64(
        &loader_test_buffer[40],
        0
    );

    loader_test_write_u32(
        &loader_test_buffer[48],
        0
    );

    loader_test_write_u16(
        &loader_test_buffer[52],
        LOADER_TEST_HEADER_SIZE
    );

    loader_test_write_u16(
        &loader_test_buffer[54],
        LOADER_TEST_PH_SIZE
    );

    loader_test_write_u16(
        &loader_test_buffer[56],
        2
    );

    /*
     * Code segment: readable and executable, not writable.
     */
    loader_test_write_program_header(
        0,
        ELF64_PF_R | ELF64_PF_X,
        0x1000,
        LOADER_TEST_CODE_VA,
        4,
        ELF64_PAGE_SIZE
    );

    /*
     * Data segment: readable and writable, not executable.
     */
    loader_test_write_program_header(
        1,
        ELF64_PF_R | ELF64_PF_W,
        0x2000,
        LOADER_TEST_DATA_VA,
        4,
        ELF64_PAGE_SIZE
    );

    /*
     * Recognizable machine-code bytes:
     *     NOP
     *     NOP
     *     NOP
     *     RET
     */
    loader_test_buffer[0x1000] = 0x90;
    loader_test_buffer[0x1001] = 0x90;
    loader_test_buffer[0x1002] = 0x90;
    loader_test_buffer[0x1003] = 0xC3;

    /*
     * Recognizable data bytes.
     */
    loader_test_buffer[0x2000] = 0x11;
    loader_test_buffer[0x2001] = 0x22;
    loader_test_buffer[0x2002] = 0x33;
    loader_test_buffer[0x2003] = 0x44;
}

/*
 * Construct a valid three-page executable whose pages
 * occupy three different 2 MiB page-table regions.
 *
 * Code:
 *     VA       0x00400000
 *     Flags    R | X
 *
 * Writable data:
 *     VA       0x00600000
 *     Flags    R | W
 *
 * Read-only data:
 *     VA       0x00800000
 *     Flags    R
 *
 * All segments occupy exactly one 4 KiB page.
 *
 * Each virtual address needs its own PT page table,
 * while all three share their higher-level page tables.
 */
static void loader_test_build_three_branch_image(void) {

    for (uint32_t i = 0;
         i < LOADER_TEST_BUFFER_SIZE;
         i++) {

        loader_test_buffer[i] = 0;
    }

    /*
     * ELF identification.
     */
    loader_test_buffer[0] = 0x7F;
    loader_test_buffer[1] = 'E';
    loader_test_buffer[2] = 'L';
    loader_test_buffer[3] = 'F';

    loader_test_buffer[4] = 2;
    loader_test_buffer[5] = 1;
    loader_test_buffer[6] = 1;

    /*
     * ELF header.
     */
    loader_test_write_u16(
        &loader_test_buffer[16],
        LOADER_TEST_ET_EXEC
    );

    loader_test_write_u16(
        &loader_test_buffer[18],
        LOADER_TEST_EM_X86_64
    );

    loader_test_write_u32(
        &loader_test_buffer[20],
        1
    );

    loader_test_write_u64(
        &loader_test_buffer[24],
        LOADER_TEST_ENTRY
    );

    loader_test_write_u64(
        &loader_test_buffer[32],
        LOADER_TEST_PH_OFFSET
    );

    loader_test_write_u64(
        &loader_test_buffer[40],
        0
    );

    loader_test_write_u32(
        &loader_test_buffer[48],
        0
    );

    loader_test_write_u16(
        &loader_test_buffer[52],
        LOADER_TEST_HEADER_SIZE
    );

    loader_test_write_u16(
        &loader_test_buffer[54],
        LOADER_TEST_PH_SIZE
    );

    loader_test_write_u16(
        &loader_test_buffer[56],
        3
    );

    /*
     * Segment 0: executable code.
     */
    loader_test_write_program_header(
        0,
        ELF64_PF_R | ELF64_PF_X,
        0x1000,
        LOADER_TEST_CODE_VA,
        4,
        ELF64_PAGE_SIZE
    );

    /*
     * Segment 1: writable, non-executable data.
     */
    loader_test_write_program_header(
        1,
        ELF64_PF_R | ELF64_PF_W,
        0x2000,
        LOADER_TEST_BRANCH_DATA_VA,
        4,
        ELF64_PAGE_SIZE
    );

    /*
     * Segment 2: read-only, non-executable data.
     */
    loader_test_write_program_header(
        2,
        ELF64_PF_R,
        0x3000,
        LOADER_TEST_BRANCH_READONLY_VA,
        4,
        ELF64_PAGE_SIZE
    );

    /*
     * Recognizable code.
     */
    loader_test_buffer[0x1000] = 0x90;
    loader_test_buffer[0x1001] = 0x90;
    loader_test_buffer[0x1002] = 0x90;
    loader_test_buffer[0x1003] = 0xC3;

    /*
     * Recognizable writable data.
     */
    loader_test_buffer[0x2000] = 0x11;
    loader_test_buffer[0x2001] = 0x22;
    loader_test_buffer[0x2002] = 0x33;
    loader_test_buffer[0x2003] = 0x44;

    /*
     * Recognizable read-only data.
     */
    loader_test_buffer[0x3000] = 0xDE;
    loader_test_buffer[0x3001] = 0xAD;
    loader_test_buffer[0x3002] = 0xBE;
    loader_test_buffer[0x3003] = 0xEF;
}

/*
 * Inspect all three pages in the multi-branch fixture.
 *
 * Returns false if a query encounters an invalid page-table
 * state or if a present mapping is not a valid 4 KiB user page.
 *
 * mapping_count_out receives the number of present mappings.
 *
 * single_physical_out receives the physical address when
 * exactly one mapping exists; otherwise it is zero.
 */
static bool loader_test_inspect_three_branch_mappings(
    vmm_address_space_t *address_space,
    uint64_t *mapping_count_out,
    uint64_t *single_physical_out
) {
    if (address_space == NULL ||
        mapping_count_out == NULL ||
        single_physical_out == NULL) {

        return false;
    }

    static const uint64_t addresses[] = {
        LOADER_TEST_CODE_VA,
        LOADER_TEST_BRANCH_DATA_VA,
        LOADER_TEST_BRANCH_READONLY_VA
    };

    uint64_t mapping_count = 0;
    uint64_t single_physical = 0;

    for (uint64_t i = 0;
         i < sizeof(addresses) / sizeof(addresses[0]);
         i++) {

        vmm_mapping_info_t mapping = {0};

        vmm_status_t status =
            vmm_address_space_query(
                address_space,
                addresses[i],
                &mapping
            );

        if (status == VMM_STATUS_NOT_MAPPED) {
            continue;
        }

        if (status != VMM_STATUS_OK) {
            return false;
        }

        if (mapping.page_size != ELF64_PAGE_SIZE ||
            mapping.huge_page ||
            !mapping.user_accessible ||
            mapping.physical_address == 0) {

            return false;
        }

        mapping_count++;

        if (mapping_count == 1) {
            single_physical =
                mapping.physical_address;
        }
    }

    *mapping_count_out = mapping_count;

    *single_physical_out =
        mapping_count == 1
            ? single_physical
            : 0;

    return true;
}

/*
 * Remove a test process and release the caller's reference.
 */
static bool loader_test_destroy_process(
    process_t *process
) {
    if (process == NULL) {
        return false;
    }

    bool destroyed =
        process_destroy(process);

    process_put(process);

    return destroyed;
}


/*
 * Verify the complete loaded image:
 *
 * - Entry point.
 * - Physical page identity.
 * - User accessibility.
 * - Code is read-only and executable.
 * - Data is writable and non-executable.
 * - File bytes are copied correctly.
 * - Remaining bytes in each page are zero.
 * - The gap page remains unmapped.
 */
static bool loader_test_verify_loaded_image(
    process_t *process,
    uint64_t entry,
    uint64_t *code_physical_out,
    uint64_t *data_physical_out
) {
    if (process == NULL ||
        code_physical_out == NULL ||
        data_physical_out == NULL) {

        return false;
    }

    *code_physical_out = 0;
    *data_physical_out = 0;

    if (entry != LOADER_TEST_ENTRY) {
        return false;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        return false;
    }

    vmm_mapping_info_t code_mapping = {0};
    vmm_mapping_info_t data_mapping = {0};
    vmm_mapping_info_t gap_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            LOADER_TEST_CODE_VA,
            &code_mapping
        ) != VMM_STATUS_OK) {

        return false;
    }

    if (vmm_address_space_query(
            address_space,
            LOADER_TEST_DATA_VA,
            &data_mapping
        ) != VMM_STATUS_OK) {

        return false;
    }

    /*
     * The gap is not part of either PT_LOAD segment.
     */
    if (vmm_address_space_query(
            address_space,
            LOADER_TEST_GAP_VA,
            &gap_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        return false;
    }

    /*
     * Verify page identity and effective permissions.
     */
    if (code_mapping.page_size != ELF64_PAGE_SIZE ||
        data_mapping.page_size != ELF64_PAGE_SIZE ||
        code_mapping.huge_page ||
        data_mapping.huge_page ||
        code_mapping.physical_address == 0 ||
        data_mapping.physical_address == 0 ||
        code_mapping.physical_address ==
            data_mapping.physical_address ||
        !code_mapping.user_accessible ||
        !data_mapping.user_accessible ||
        code_mapping.writable ||
        !code_mapping.executable ||
        !data_mapping.writable ||
        data_mapping.executable) {

        return false;
    }

    /*
     * Read the contents through the kernel HHDM aliases.
     */
    uint8_t *code =
        (uint8_t *)pmm_frame_to_virt(
            code_mapping.physical_address
        );

    uint8_t *data =
        (uint8_t *)pmm_frame_to_virt(
            data_mapping.physical_address
        );

    if (code == NULL || data == NULL) {
        return false;
    }

    const uint8_t expected_code[4] = {
        0x90, 0x90, 0x90, 0xC3
    };

    const uint8_t expected_data[4] = {
        0x11, 0x22, 0x33, 0x44
    };

    for (uint64_t i = 0; i < 4; i++) {

        if (code[i] != expected_code[i] ||
            data[i] != expected_data[i]) {

            return false;
        }
    }

    /*
     * The rest of each page must be zero.
     *
     * This verifies BSS/zero-fill behavior and prevents stale
     * physical-memory contents from becoming visible to user mode.
     */
    for (uint64_t i = 4;
         i < ELF64_PAGE_SIZE;
         i++) {

        if (code[i] != 0 ||
            data[i] != 0) {

            return false;
        }
    }

    *code_physical_out =
        code_mapping.physical_address;

    *data_physical_out =
        data_mapping.physical_address;

    return true;
}


/*
 * Test 1:
 *
 * Load a valid executable and verify its mapped pages,
 * contents, permissions, and process cleanup.
 */
static bool loader_test_successful_load(void) {

    loader_test_build_valid_image();

    process_t *process =
        process_create_user("elf-loader-success");

    if (process == NULL) {
        return false;
    }

    uint64_t entry = UINT64_MAX;

    elf64_status_t elf_status =
        ELF64_STATUS_INVALID_ARGUMENT;

    elf64_loader_status_t status =
        elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        );

    uint64_t code_physical = 0;
    uint64_t data_physical = 0;

    bool image_ok =
        status == ELF64_LOADER_STATUS_OK &&
        elf_status == ELF64_STATUS_OK &&
        loader_test_verify_loaded_image(
            process,
            entry,
            &code_physical,
            &data_physical
        );

    bool destroyed =
        loader_test_destroy_process(process);

    /*
     * The final process reference must release its image frames.
     */
    bool frames_released =
        code_physical != 0 &&
        data_physical != 0 &&
        pmm_frame_to_virt(code_physical) == NULL &&
        pmm_frame_to_virt(data_physical) == NULL;

    return image_ok &&
        destroyed &&
        frames_released;
}


/*
 * Test 2:
 *
 * A malformed/unsupported ELF must not reserve or poison
 * the target process.
 *
 * After rejection, the same process must accept a valid image.
 */
static bool loader_test_rejected_image_can_retry(void) {

    loader_test_build_valid_image();

    /*
     * Convert ET_EXEC into ET_DYN.
     * The current loader deliberately does not support PIE.
     */
    loader_test_write_u16(
        &loader_test_buffer[16],
        LOADER_TEST_ET_DYN
    );

    process_t *process =
        process_create_user("elf-loader-retry");

    if (process == NULL) {
        return false;
    }

    uint64_t entry = 0xAABBCCDDEEFF0011ULL;

    elf64_status_t elf_status =
        ELF64_STATUS_OK;

    elf64_loader_status_t rejected_status =
        elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        );

    bool rejected_correctly =
        rejected_status ==
            ELF64_LOADER_STATUS_PARSE_FAILED &&
        elf_status == ELF64_STATUS_UNSUPPORTED &&
        entry == 0xAABBCCDDEEFF0011ULL;

    /*
     * Restore a valid executable and retry on the same process.
     */
    loader_test_build_valid_image();

    elf_status = ELF64_STATUS_INVALID_ARGUMENT;

    elf64_loader_status_t retry_status =
        elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        );

    uint64_t code_physical = 0;
    uint64_t data_physical = 0;

    bool retry_succeeded =
        retry_status == ELF64_LOADER_STATUS_OK &&
        elf_status == ELF64_STATUS_OK &&
        loader_test_verify_loaded_image(
            process,
            entry,
            &code_physical,
            &data_physical
        );

    bool destroyed =
        loader_test_destroy_process(process);

    bool frames_released =
        code_physical != 0 &&
        data_physical != 0 &&
        pmm_frame_to_virt(code_physical) == NULL &&
        pmm_frame_to_virt(data_physical) == NULL;

    return rejected_correctly &&
        retry_succeeded &&
        destroyed &&
        frames_released;
}


/*
 * Test 3:
 *
 * An existing virtual mapping must be detected before ELF
 * frames are allocated or the existing mapping is modified.
 *
 * After removing the collision, the same process must be
 * eligible for a successful retry.
 */
static bool loader_test_mapping_collision_and_retry(void) {

    loader_test_build_valid_image();

    process_t *process =
        process_create_user("elf-loader-collision");

    if (process == NULL) {
        return false;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        (void)loader_test_destroy_process(process);
        return false;
    }

    /*
     * Create an unrelated mapping at the executable's entry page.
     */
    uint64_t collision_frame =
        pmm_alloc_frame();

    if (collision_frame == 0) {
        (void)loader_test_destroy_process(process);
        return false;
    }

    vmm_status_t map_status =
        vmm_address_space_map_page(
            address_space,
            LOADER_TEST_CODE_VA,
            collision_frame,
            VMM_MAP_USER
        );

    if (map_status != VMM_STATUS_OK) {

        (void)pmm_free_frame(collision_frame);

        (void)loader_test_destroy_process(process);

        return false;
    }

    uint64_t entry =
        0xAABBCCDDEEFF0011ULL;

    elf64_status_t elf_status =
        ELF64_STATUS_INVALID_ARGUMENT;

    elf64_loader_status_t load_status =
        elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        );

    /*
     * The collision must remain exactly where it was.
     */
    vmm_mapping_info_t collision_mapping = {0};
    vmm_mapping_info_t data_mapping = {0};

    bool collision_intact =
        vmm_address_space_query(
            address_space,
            LOADER_TEST_CODE_VA,
            &collision_mapping
        ) == VMM_STATUS_OK &&
        collision_mapping.physical_address ==
            collision_frame &&
        collision_mapping.user_accessible &&
        !collision_mapping.writable &&
        !collision_mapping.executable;

    bool data_still_unmapped =
        vmm_address_space_query(
            address_space,
            LOADER_TEST_DATA_VA,
            &data_mapping
        ) == VMM_STATUS_NOT_MAPPED;

    bool rejected_without_modification =
        load_status ==
            ELF64_LOADER_STATUS_ADDRESS_CONFLICT &&
        elf_status == ELF64_STATUS_OK &&
        entry == 0xAABBCCDDEEFF0011ULL &&
        collision_intact &&
        data_still_unmapped;

    /*
     * Remove only the unrelated mapping.
     */
    uint64_t unmapped_frame = 0;

    vmm_status_t unmap_status =
        vmm_address_space_unmap_page(
            address_space,
            LOADER_TEST_CODE_VA,
            &unmapped_frame
        );

    bool collision_removed =
        unmap_status == VMM_STATUS_OK &&
        unmapped_frame == collision_frame;

    bool collision_frame_freed =
        collision_removed &&
        pmm_free_frame(collision_frame);

    /*
     * The failed preflight must have ended image_loading.
     * A valid retry must now work on the same process.
     */
    entry = UINT64_MAX;
    elf_status = ELF64_STATUS_INVALID_ARGUMENT;

    elf64_loader_status_t retry_status =
        elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        );

    uint64_t code_physical = 0;
    uint64_t data_physical = 0;

    bool retry_succeeded =
        collision_frame_freed &&
        retry_status == ELF64_LOADER_STATUS_OK &&
        elf_status == ELF64_STATUS_OK &&
        loader_test_verify_loaded_image(
            process,
            entry,
            &code_physical,
            &data_physical
        );

    bool destroyed =
        loader_test_destroy_process(process);

    bool image_frames_released =
        code_physical != 0 &&
        data_physical != 0 &&
        pmm_frame_to_virt(code_physical) == NULL &&
        pmm_frame_to_virt(data_physical) == NULL;

    return rejected_without_modification &&
        collision_frame_freed &&
        retry_succeeded &&
        destroyed &&
        image_frames_released;
}

/*
 * Test:
 *
 * Inject a mapping failure after:
 *     0 successful mappings,
 *     1 successful mapping,
 *     2 successful mappings.
 *
 * Every failed load must:
 *     - Preserve entry_out.
 *     - Report MAPPING_FAILED.
 *     - Report ELF preparation success.
 *     - Remove all partially installed mappings.
 *     - Complete owned-frame cleanup.
 */
static bool loader_test_mapping_failure_matrix(void) {

    const uint64_t entry_sentinel =
        0xAABBCCDDEEFF0011ULL;

    for (uint64_t successful_maps = 0;
         successful_maps < 3;
         successful_maps++) {

        loader_test_build_three_branch_image();

        process_t *process =
            process_create_user(
                "elf-map-failure-matrix"
            );

        if (process == NULL) {
            return false;
        }

        vmm_address_space_t *address_space = NULL;

        if (!process_get_address_space(
                process,
                &address_space
            ) ||
            address_space == NULL) {

            (void)loader_test_destroy_process(process);
            return false;
        }

        vmm_test_clear_address_space_map_failure();
        vmm_test_clear_address_space_unmap_failure();
        vmm_test_clear_table_allocation_failure();

        pmm_test_clear_free_frame_failure();
        pmm_test_clear_free_after_failure();

        uint64_t entry = entry_sentinel;

        elf64_status_t elf_status =
            ELF64_STATUS_INVALID_ARGUMENT;

        vmm_test_fail_address_space_map_after(
            successful_maps
        );

        elf64_loader_status_t status =
            elf64_load_process(
                process,
                loader_test_buffer,
                sizeof(loader_test_buffer),
                &entry,
                &elf_status
            );

        vmm_test_clear_address_space_map_failure();

        uint64_t mapping_count = UINT64_MAX;
        uint64_t single_physical = UINT64_MAX;

        bool mapping_state_valid =
            loader_test_inspect_three_branch_mappings(
                address_space,
                &mapping_count,
                &single_physical
            );

        bool rollback_correct =
            status == ELF64_LOADER_STATUS_MAPPING_FAILED &&
            elf_status == ELF64_STATUS_OK &&
            entry == entry_sentinel &&
            mapping_state_valid &&
            mapping_count == 0 &&
            single_physical == 0;

        /*
         * The loader should already have released the
         * complete attached page set after successful rollback.
         */
        bool destroyed =
            loader_test_destroy_process(process);

        if (!rollback_correct || !destroyed) {
            return false;
        }
    }

    return true;
}

/*
 * Test:
 *
 * A fresh process address space needs three intermediate
 * page-table frames for its first user mapping.
 *
 * Each additional 2 MiB branch needs one more PT frame.
 *
 * Failure positions:
 *     0: fail allocating the PDPT.
 *     1: fail allocating the page directory.
 *     2: fail allocating the first page table.
 *     3: allow the first mapping, fail the second PT.
 *     4: allow two mappings, fail the third PT.
 *
 * Each failure must leave no user mappings behind.
 */
static bool loader_test_table_allocation_failure_matrix(void) {

    const uint64_t entry_sentinel =
        0x1122334455667788ULL;

    for (uint64_t successful_allocations = 0;
         successful_allocations < 5;
         successful_allocations++) {

        loader_test_build_three_branch_image();

        process_t *process =
            process_create_user(
                "elf-table-allocation-matrix"
            );

        if (process == NULL) {
            return false;
        }

        vmm_address_space_t *address_space = NULL;

        if (!process_get_address_space(
                process,
                &address_space
            ) ||
            address_space == NULL) {

            (void)loader_test_destroy_process(process);
            return false;
        }

        vmm_test_clear_address_space_map_failure();
        vmm_test_clear_address_space_unmap_failure();
        vmm_test_clear_table_allocation_failure();

        pmm_test_clear_free_frame_failure();
        pmm_test_clear_free_after_failure();

        uint64_t entry = entry_sentinel;

        elf64_status_t elf_status =
            ELF64_STATUS_INVALID_ARGUMENT;

        /*
         * This hook is armed after process creation, so it
         * applies to the ELF's page-table creation only.
         */
        vmm_test_fail_table_allocation_after(
            successful_allocations
        );

        elf64_loader_status_t status =
            elf64_load_process(
                process,
                loader_test_buffer,
                sizeof(loader_test_buffer),
                &entry,
                &elf_status
            );

        vmm_test_clear_table_allocation_failure();

        uint64_t mapping_count = UINT64_MAX;
        uint64_t single_physical = UINT64_MAX;

        bool mapping_state_valid =
            loader_test_inspect_three_branch_mappings(
                address_space,
                &mapping_count,
                &single_physical
            );

        bool rollback_correct =
            status == ELF64_LOADER_STATUS_MAPPING_FAILED &&
            elf_status == ELF64_STATUS_OK &&
            entry == entry_sentinel &&
            mapping_state_valid &&
            mapping_count == 0 &&
            single_physical == 0;

        /*
         * This also exercises address-space teardown after
         * each intermediate page-table allocation failure.
         */
        bool destroyed =
            loader_test_destroy_process(process);

        if (!rollback_correct || !destroyed) {
            return false;
        }
    }

    return true;
}

/*
 * Test:
 *
 * Allow one image page to map, then fail the next image mapping.
 *
 * During rollback, fail the first unmap operation.
 *
 * Required behavior:
 *     - Loader returns CLEANUP_FAILED.
 *     - Exactly one mapping remains.
 *     - Its physical frame remains allocated.
 *     - entry_out remains unchanged.
 *     - The page set remains process-owned.
 *     - Destroying the process retries cleanup and releases it.
 */
static bool loader_test_rollback_unmap_failure_retry(void) {

    loader_test_build_three_branch_image();

    process_t *process =
        process_create_user(
            "elf-unmap-failure-retry"
        );

    if (process == NULL) {
        return false;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        (void)loader_test_destroy_process(process);
        return false;
    }

    const uint64_t entry_sentinel =
        0xCAFEBABE12345678ULL;

    uint64_t entry = entry_sentinel;

    elf64_status_t elf_status =
        ELF64_STATUS_INVALID_ARGUMENT;

    vmm_test_clear_address_space_map_failure();
    vmm_test_clear_address_space_unmap_failure();
    vmm_test_clear_table_allocation_failure();

    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * First mapping succeeds. The next map fails.
     */
    vmm_test_fail_address_space_map_after(1);

    /*
     * The first actual unmap attempted by rollback fails,
     * regardless of which virtual address was mapped first.
     */
    vmm_test_fail_next_address_space_unmap_once();

    elf64_loader_status_t status =
        elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        );

    vmm_test_clear_address_space_map_failure();
    vmm_test_clear_address_space_unmap_failure();

    uint64_t mapping_count = UINT64_MAX;
    uint64_t retained_physical = 0;

    bool mapping_state_valid =
        loader_test_inspect_three_branch_mappings(
            address_space,
            &mapping_count,
            &retained_physical
        );

    bool ownership_retained =
        status == ELF64_LOADER_STATUS_CLEANUP_FAILED &&
        elf_status == ELF64_STATUS_OK &&
        entry == entry_sentinel &&
        mapping_state_valid &&
        mapping_count == 1 &&
        retained_physical != 0 &&
        pmm_frame_to_virt(retained_physical) != NULL;

    /*
     * The injected unmap failure is one-shot and has been
     * cleared. Final process cleanup should now succeed.
     */
    bool destroyed =
        loader_test_destroy_process(process);

    bool retained_frame_released =
        retained_physical != 0 &&
        pmm_frame_to_virt(retained_physical) == NULL;

    return ownership_retained &&
        destroyed &&
        retained_frame_released;
}

/*
 * Test:
 *
 * Allow two mappings, then fail the third mapping.
 *
 * During rollback:
 *     - Fail the first unmap.
 *     - Allow the second unmap.
 *     - Allow one PMM frame release.
 *     - Fail the next PMM frame release.
 *
 * The three segments occupy different PDE branches.
 *
 * Consequently, after the first unmap failure, the second
 * successful unmap should reclaim exactly its now-empty PT.
 * That is the one successful PMM release permitted by the
 * countdown hook. The following data-frame release then fails.
 *
 * Required behavior:
 *     - Exactly one mapping remains.
 *     - The failed-free physical frame remains allocated.
 *     - Both retained frames are released by final process cleanup.
 *     - entry_out remains unchanged.
 */
static bool loader_test_combined_rollback_failure_retry(void) {

    loader_test_build_three_branch_image();

    process_t *process =
        process_create_user(
            "elf-combined-rollback-retry"
        );

    if (process == NULL) {
        return false;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        (void)loader_test_destroy_process(process);
        return false;
    }

    const uint64_t entry_sentinel =
        0xDEADC0DE87654321ULL;

    uint64_t entry = entry_sentinel;

    elf64_status_t elf_status =
        ELF64_STATUS_INVALID_ARGUMENT;

    uint64_t failed_free_physical = 0;
    uint64_t retained_mapping_physical = 0;

    bool rollback_state_correct = false;

    vmm_test_clear_address_space_map_failure();
    vmm_test_clear_address_space_unmap_failure();
    vmm_test_clear_table_allocation_failure();

    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * The first two mappings succeed. The third map fails
     * before its PTE is modified.
     */
    vmm_test_fail_address_space_map_after(2);

    /*
     * Retain the first mapped page by failing its unmap.
     */
    vmm_test_fail_next_address_space_unmap_once();

    /*
     * The next unmap should remove a page in a different PDE.
     * Its now-empty PT should be the first PMM frame released.
     *
     * Allow that one table-frame release, then fail the
     * following physical-frame release.
     */
    if (!pmm_test_fail_free_after(1)) {
        goto cleanup;
    }

    elf64_loader_status_t status =
        elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        );

    /*
     * Capture the injected failure address before clearing
     * the PMM hook. The recorded address survives clearing.
     */
    failed_free_physical =
        pmm_test_get_last_failed_free_frame();

    vmm_test_clear_address_space_map_failure();
    vmm_test_clear_address_space_unmap_failure();
    vmm_test_clear_table_allocation_failure();

    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    uint64_t mapping_count = UINT64_MAX;

    bool mapping_state_valid =
        loader_test_inspect_three_branch_mappings(
            address_space,
            &mapping_count,
            &retained_mapping_physical
        );

    /*
     * One physical frame is still mapped.
     *
     * The failed PMM release belongs to a different frame:
     * it is unmapped but must remain allocated and owned.
     */
    rollback_state_correct =
        status == ELF64_LOADER_STATUS_CLEANUP_FAILED &&
        elf_status == ELF64_STATUS_OK &&
        entry == entry_sentinel &&
        mapping_state_valid &&
        mapping_count == 1 &&
        retained_mapping_physical != 0 &&
        failed_free_physical != 0 &&
        failed_free_physical != retained_mapping_physical &&
        pmm_frame_to_virt(retained_mapping_physical) != NULL &&
        pmm_frame_to_virt(failed_free_physical) != NULL;

cleanup:

    /*
     * Always disarm every failure hook before final cleanup.
     */
    vmm_test_clear_address_space_map_failure();
    vmm_test_clear_address_space_unmap_failure();
    vmm_test_clear_table_allocation_failure();

    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * process_put() retries all retained image-page cleanup.
     * It also releases the private address space when possible.
     */
    bool destroyed =
        loader_test_destroy_process(process);

    bool retained_mapping_frame_released =
        retained_mapping_physical != 0 &&
        pmm_frame_to_virt(retained_mapping_physical) == NULL;

    bool failed_free_frame_released =
        failed_free_physical != 0 &&
        pmm_frame_to_virt(failed_free_physical) == NULL;

    return rollback_state_correct &&
        destroyed &&
        retained_mapping_frame_released &&
        failed_free_frame_released;
}

/*
 * Run process-integrated ELF64 loader tests.
 */
bool elf64_loader_run_tests(void) {

    console_write("[TEST] Process-integrated ELF64 loader\n");

    bool successful_load =
        loader_test_successful_load();

    console_write(successful_load
        ? "  [ OK ] Load executable, permissions, BSS, and cleanup\n"
        : "  [FAIL] Load executable, permissions, BSS, and cleanup\n");

    bool invalid_retry =
        loader_test_rejected_image_can_retry();

    console_write(invalid_retry
        ? "  [ OK ] Reject unsupported ELF and retry process\n"
        : "  [FAIL] Reject unsupported ELF and retry process\n");

    bool collision_retry =
        loader_test_mapping_collision_and_retry();

    console_write(collision_retry
        ? "  [ OK ] Mapping collision preserves existing mapping\n"
        : "  [FAIL] Mapping collision preserves existing mapping\n");

    bool mapping_failure_matrix =
        loader_test_mapping_failure_matrix();

    console_write(mapping_failure_matrix
        ? "  [ OK ] Mapping failure at 0/1/2 pages rolls back\n"
        : "  [FAIL] Mapping failure at 0/1/2 pages rolls back\n");

    bool table_allocation_matrix =
        loader_test_table_allocation_failure_matrix();

    console_write(table_allocation_matrix
        ? "  [ OK ] Page-table allocation failure matrix (0-4)\n"
        : "  [FAIL] Page-table allocation failure matrix (0-4)\n");

    bool unmap_failure_retry =
        loader_test_rollback_unmap_failure_retry();

    console_write(unmap_failure_retry
        ? "  [ OK ] Rollback unmap failure retains ownership and retries\n"
        : "  [FAIL] Rollback unmap failure retains ownership and retries\n");

    bool combined_rollback_retry =
        loader_test_combined_rollback_failure_retry();

    console_write(combined_rollback_retry
        ? "  [ OK ] Combined unmap/frame-free failure retains and retries\n"
        : "  [FAIL] Combined unmap/frame-free failure retains and retries\n");

    bool passed =
        successful_load &&
        invalid_retry &&
        collision_retry &&
        mapping_failure_matrix &&
        table_allocation_matrix &&
        unmap_failure_retry &&
        combined_rollback_retry;

    console_write(passed
        ? "[ OK ] Process-integrated ELF64 loader tests passed\n"
        : "[FAIL] Process-integrated ELF64 loader tests failed\n");

    return passed;
}

/*
 * Build a small real Ring 3 program.
 *
 * It verifies:
 *   1. RFLAGS.IF is set on initial user entry.
 *   2. Initial RSP is 16-byte aligned.
 *   3. The initial stack is writable.
 *   4. GET_ABI_VERSION returns the expected ABI version.
 *
 * It then exits through HUA_SYSCALL_PROCESS_EXIT.
 *
 * Any failed check exits with LOADER_LAUNCH_FAILURE_EXIT_CODE.
 */
static void loader_test_build_user_launch_image(void) {

    loader_test_build_valid_image();

    uint8_t code[106] = {

        /* 0: pushfq */
        0x9C,

        /* 1: pop rax */
        0x58,

        /* 2: and rax, 0x200 */
        0x48, 0x25, 0x00, 0x02, 0x00, 0x00,

        /* 8: cmp rax, 0x200 */
        0x48, 0x3D, 0x00, 0x02, 0x00, 0x00,

        /* 14: jne failure */
        0x0F, 0x85, 0x43, 0x00, 0x00, 0x00,

        /* 20: mov rax, rsp */
        0x48, 0x89, 0xE0,

        /* 23: and rax, 0xF */
        0x48, 0x83, 0xE0, 0x0F,

        /* 27: jne failure */
        0x0F, 0x85, 0x36, 0x00, 0x00, 0x00,

        /* 33: mov qword [rsp], 0 */
        0x48, 0xC7, 0x04, 0x24,
        0x00, 0x00, 0x00, 0x00,

        /* 41: cmp qword [rsp], 0 */
        0x48, 0x83, 0x3C, 0x24, 0x00,

        /* 46: jne failure */
        0x0F, 0x85, 0x23, 0x00, 0x00, 0x00,

        /* 52: mov eax, HUA_SYSCALL_GET_ABI_VERSION */
        0xB8, 0x00, 0x00, 0x00, 0x00,

        /* 57: syscall */
        0x0F, 0x05,

        /* 59: cmp eax, HUA_SYSCALL_ABI_VERSION */
        0x83, 0xF8, 0x01,

        /* 62: jne failure */
        0x0F, 0x85, 0x13, 0x00, 0x00, 0x00,

        /* 68: mov eax, HUA_SYSCALL_PROCESS_EXIT */
        0xB8, 0x05, 0x00, 0x00, 0x00,

        /* 73: mov rdi, success exit code */
        0x48, 0xBF,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,

        /* 83: syscall */
        0x0F, 0x05,

        /* 85: ud2 -- PROCESS_EXIT must never return */
        0x0F, 0x0B,

        /* 87: failure */
        /* mov eax, HUA_SYSCALL_PROCESS_EXIT */
        0xB8, 0x05, 0x00, 0x00, 0x00,

        /* 92: mov rdi, failure exit code */
        0x48, 0xBF,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,

        /* 102: syscall */
        0x0F, 0x05,

        /* 104: ud2 */
        0x0F, 0x0B
    };

    loader_test_write_u64(
        &code[75],
        (uint64_t)LOADER_LAUNCH_SUCCESS_EXIT_CODE
    );

    loader_test_write_u64(
        &code[94],
        (uint64_t)LOADER_LAUNCH_FAILURE_EXIT_CODE
    );

    /*
     * The original test image used a four-byte code segment.
     * Expand its file size for this generated program.
     */
    loader_test_write_program_header(
        0,
        ELF64_PF_R | ELF64_PF_X,
        0x1000,
        LOADER_TEST_CODE_VA,
        sizeof(code),
        ELF64_PAGE_SIZE
    );

    for (uint64_t i = 0; i < sizeof(code); i++) {
        loader_test_buffer[0x1000ULL + i] = code[i];
    }
}

static bool loader_test_user_stack_collision_retry(void) {

    loader_test_build_valid_image();

    process_t *process =
        process_create_user("user-stack-collision");

    if (process == NULL) {
        return false;
    }

    bool success = false;

    uint64_t collision_physical = 0;
    bool collision_mapped = false;

    uint64_t entry = 0;

    elf64_status_t elf_status = ELF64_STATUS_OK;

    if (elf64_load_process(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &elf_status
        ) != ELF64_LOADER_STATUS_OK ||
        elf_status != ELF64_STATUS_OK) {

        goto cleanup;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        goto cleanup;
    }

    /*
     * Install an unrelated mapping at the first stack page.
     * The stack builder must detect it before allocating frames.
     */
    collision_physical = pmm_alloc_frame();

    if (collision_physical == 0) {
        goto cleanup;
    }

    if (vmm_address_space_map_page(
            address_space,
            ELF64_USER_STACK_BASE,
            collision_physical,
            VMM_MAP_USER | VMM_MAP_WRITABLE
        ) != VMM_STATUS_OK) {

        (void)pmm_free_frame(collision_physical);
        goto cleanup;
    }

    collision_mapped = true;

    uint64_t user_rsp = 0xAABBCCDDEEFF0011ULL;

    elf64_loader_status_t collision_status =
        elf64_prepare_user_stack(
            process,
            &user_rsp
        );

    bool rejected =
        collision_status ==
            ELF64_LOADER_STATUS_ADDRESS_CONFLICT &&
        user_rsp == 0xAABBCCDDEEFF0011ULL;

    /*
     * Remove the foreign mapping without changing its ownership.
     */
    uint64_t unmapped_physical = 0;

    bool unmapped =
        vmm_address_space_unmap_page(
            address_space,
            ELF64_USER_STACK_BASE,
            &unmapped_physical
        ) == VMM_STATUS_OK &&
        unmapped_physical == collision_physical;

    bool frame_released =
        unmapped &&
        pmm_free_frame(unmapped_physical);

    if (!rejected || !frame_released) {
        goto cleanup;
    }

    collision_mapped = false;

    /*
     * The same loaded process should now be eligible for
     * stack construction again.
     */
    elf64_loader_status_t retry_status =
        elf64_prepare_user_stack(
            process,
            &user_rsp
        );

    if (retry_status != ELF64_LOADER_STATUS_OK ||
        user_rsp !=
            ELF64_USER_STACK_TOP -
            ELF64_USER_INITIAL_STACK_SIZE) {

        goto cleanup;
    }

    /*
     * Verify the guard page remains unmapped.
     */
    vmm_mapping_info_t guard_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            ELF64_USER_STACK_GUARD,
            &guard_mapping
        ) != VMM_STATUS_NOT_MAPPED) {

        goto cleanup;
    }

    /*
     * Verify every stack page's actual hardware permissions.
     */
    for (uint64_t address = ELF64_USER_STACK_BASE;
         address < ELF64_USER_STACK_TOP;
         address += ELF64_PAGE_SIZE) {

        vmm_mapping_info_t mapping = {0};

        if (vmm_address_space_query(
                address_space,
                address,
                &mapping
            ) != VMM_STATUS_OK ||
            !mapping.user_accessible ||
            !mapping.writable ||
            mapping.executable ||
            mapping.page_size != ELF64_PAGE_SIZE) {

            goto cleanup;
        }
    }

    success = true;

cleanup:

    if (collision_mapped) {

        uint64_t unmapped_physical = 0;

        if (vmm_address_space_unmap_page(
                address_space,
                ELF64_USER_STACK_BASE,
                &unmapped_physical
            ) != VMM_STATUS_OK ||
            unmapped_physical != collision_physical ||
            !pmm_free_frame(unmapped_physical)) {

            /*
             * Do not proceed with process destruction if the
             * foreign mapping's ownership cannot be reconciled.
             */
            return false;
        }

        collision_mapped = false;
    }

    /*
     * The test process has never acquired a thread, so it should
     * still be NEW and can safely be destroyed.
     */
    if (!process_destroy(process)) {
        success = false;
    }

    process_put(process);

    return success;
}

static bool loader_test_real_process_launch(void) {

    loader_test_build_user_launch_image();

    process_t *process =
        process_create_user("real-elf-user-process");

    if (process == NULL) {
        return false;
    }

    bool launched = false;
    bool process_destroyed = false;

    bool success = false;

    uint64_t entry = UINT64_MAX;
    uint64_t user_rsp = UINT64_MAX;

    elf64_status_t elf_status =
        ELF64_STATUS_INVALID_ARGUMENT;

    elf64_loader_status_t load_status =
        elf64_load_process_with_stack(
            process,
            loader_test_buffer,
            sizeof(loader_test_buffer),
            &entry,
            &user_rsp,
            &elf_status
        );

    if (load_status != ELF64_LOADER_STATUS_OK ||
        elf_status != ELF64_STATUS_OK ||
        entry != LOADER_TEST_ENTRY ||
        user_rsp !=
            ELF64_USER_STACK_TOP -
            ELF64_USER_INITIAL_STACK_SIZE ||
        (user_rsp & 15ULL) != 0) {

        goto cleanup;
    }

    vmm_address_space_t *address_space = NULL;

    if (!process_get_address_space(
            process,
            &address_space
        ) ||
        address_space == NULL) {

        goto cleanup;
    }

    /*
     * Capture physical frames before the process is destroyed,
     * so we can independently check that all of them are released.
     */
    uint64_t code_physical = 0;
    uint64_t data_physical = 0;

    vmm_mapping_info_t code_mapping = {0};
    vmm_mapping_info_t data_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            LOADER_TEST_CODE_VA,
            &code_mapping
        ) != VMM_STATUS_OK ||
        vmm_address_space_query(
            address_space,
            LOADER_TEST_DATA_VA,
            &data_mapping
        ) != VMM_STATUS_OK) {

        goto cleanup;
    }

    code_physical = code_mapping.physical_address;
    data_physical = data_mapping.physical_address;

    if (code_physical == 0 ||
        data_physical == 0 ||
        !code_mapping.user_accessible ||
        code_mapping.writable ||
        !code_mapping.executable ||
        !data_mapping.user_accessible ||
        !data_mapping.writable ||
        data_mapping.executable) {

        goto cleanup;
    }

    uint64_t stack_physical[
        ELF64_USER_STACK_PAGE_COUNT
    ] = {0};

    uint64_t stack_index = 0;

    /*
     * Verify and remember every mapped stack frame.
     */
    for (uint64_t address = ELF64_USER_STACK_BASE;
         address < ELF64_USER_STACK_TOP;
         address += ELF64_PAGE_SIZE) {

        vmm_mapping_info_t mapping = {0};

        if (stack_index >= ELF64_USER_STACK_PAGE_COUNT ||
            vmm_address_space_query(
                address_space,
                address,
                &mapping
            ) != VMM_STATUS_OK ||
            mapping.page_size != ELF64_PAGE_SIZE ||
            !mapping.user_accessible ||
            !mapping.writable ||
            mapping.executable ||
            mapping.physical_address == 0) {

            goto cleanup;
        }

        stack_physical[stack_index++] =
            mapping.physical_address;
    }

    if (stack_index != ELF64_USER_STACK_PAGE_COUNT) {
        goto cleanup;
    }

    /*
     * Verify the initial stack contents through the kernel HHDM.
     */
    vmm_mapping_info_t last_stack_mapping = {0};

    if (vmm_address_space_query(
            address_space,
            ELF64_USER_STACK_TOP - ELF64_PAGE_SIZE,
            &last_stack_mapping
        ) != VMM_STATUS_OK) {

        goto cleanup;
    }

    uint8_t *last_stack_alias =
        (uint8_t *)pmm_frame_to_virt(
            last_stack_mapping.physical_address
        );

    if (last_stack_alias == NULL) {
        goto cleanup;
    }

    uint64_t *initial_stack =
        (uint64_t *)(
            last_stack_alias +
            ELF64_PAGE_SIZE -
            ELF64_USER_INITIAL_STACK_SIZE
        );

    for (uint64_t i = 0; i < 6; i++) {

        if (initial_stack[i] != 0) {
            goto cleanup;
        }
    }

    /*
     * Verify that invalid start parameters do not consume the
     * process's committed initial-stack permission.
     */
    if (elf64_start_process(
            process,
            entry,
            user_rsp + 8ULL
        ) != ELF64_LOADER_STATUS_INVALID_ARGUMENT) {

        goto cleanup;
    }

    /*
     * The data mapping is writable and NX, so it cannot be
     * accepted as an executable entry point.
     */
    if (elf64_start_process(
            process,
            LOADER_TEST_DATA_VA,
            user_rsp
        ) != ELF64_LOADER_STATUS_INVALID_ARGUMENT) {

        goto cleanup;
    }

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    struct thread *bootstrap =
        scheduler_get_current_thread(0);

    if (cpu == NULL ||
        bootstrap == NULL ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        goto cleanup;
    }

    /*
     * This is the actual first-thread publication operation.
     */
    elf64_loader_status_t start_status =
        elf64_start_process(
            process,
            entry,
            user_rsp
        );

    if (start_status != ELF64_LOADER_STATUS_OK) {
        goto cleanup;
    }

    launched = true;

    /*
     * Ensure the new user thread gets CPU time immediately.
     *
     * The call resumes when the bootstrap thread is scheduled
     * again. Timer preemption may also switch between threads.
     */
    if (!thread_yield()) {
        goto launched_failure;
    }

    /*
     * Wait for the final thread to terminate and be detached
     * by the deferred scheduler reaper.
     */
    uint64_t start_ticks = cpu->timer_ticks;

    bool zombie_observed = false;

    for (uint64_t iterations = 0;
         iterations < 10000000ULL;
         iterations++) {

        scheduler_reap_terminated_threads();

        process_state_t state = PROCESS_STATE_NEW;
        uint32_t thread_count = UINT32_MAX;

        if (!process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            )) {

            goto launched_failure;
        }

        if (state == PROCESS_STATE_ZOMBIE &&
            thread_count == 0) {

            zombie_observed = true;
            break;
        }

        /*
         * A functioning preemptible process must allow the
         * bootstrap thread to run again.
         */
        if (cpu->timer_ticks - start_ticks >= 1000ULL) {
            break;
        }

        asm volatile ("pause");
    }

    scheduler_reap_terminated_threads();

    if (!zombie_observed) {
        goto launched_failure;
    }

    /*
     * The process must have exited with the success code.
     *
     * The failure branch in the user executable uses a
     * different status.
     */
    int64_t exit_code = 0;

    if (!process_get_exit_code(
            process,
            &exit_code
        ) ||
        exit_code != LOADER_LAUNCH_SUCCESS_EXIT_CODE) {

        goto launched_failure;
    }

    /*
     * The scheduler must have resumed the original bootstrap
     * thread after the user thread terminated.
     */
    if (scheduler_get_current_thread(0) != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        goto launched_failure;
    }

    console_write(
        "[ OK ] Real ELF process entered Ring 3 with IF enabled\n"
    );

    console_write(
        "[ OK ] Initial user stack alignment and write access verified\n"
    );

    console_write(
        "[ OK ] User process exited through HUA_SYSCALL_PROCESS_EXIT\n"
    );

    console_write(
        "[ OK ] Scheduler reaped the final user thread\n"
    );

    /*
     * There must be no attached thread before destruction.
     */
    if (!process_destroy(process)) {
        goto launched_failure;
    }

    process_destroyed = true;

    process_put(process);
    process = NULL;

    /*
     * process_put() must have released all image and stack frames.
     */
    if (pmm_frame_to_virt(code_physical) != NULL ||
        pmm_frame_to_virt(data_physical) != NULL) {

        return false;
    }

    for (uint64_t i = 0;
         i < ELF64_USER_STACK_PAGE_COUNT;
         i++) {

        if (pmm_frame_to_virt(stack_physical[i]) != NULL) {
            return false;
        }
    }

    success = true;

    console_write(
        "[ OK ] Image, stack, and address-space resources released\n"
    );

    return success;

launched_failure:

    /*
     * Once the thread has been published, its resources must not
     * be freed until the reaper confirms process termination.
     *
     * Returning failure here deliberately leaves ownership
     * records intact rather than risking a use-after-free.
     */
    if (launched) {
        return false;
    }

cleanup:

    if (process != NULL && !process_destroyed) {

        process_state_t state = PROCESS_STATE_NEW;
        uint32_t thread_count = UINT32_MAX;

        if (process_get_info(
                process,
                NULL,
                &state,
                &thread_count,
                NULL,
                0
            ) &&
            thread_count == 0 &&
            (state == PROCESS_STATE_NEW ||
             state == PROCESS_STATE_ZOMBIE)) {

            (void)process_destroy(process);
        }

        process_put(process);
    }

    return success;
}

bool elf64_loader_launch_run_tests(void) {

    console_write(
        "\n========== REAL ELF USER-PROCESS LAUNCH TESTS ==========\n"
    );

    if (!scheduler_is_initialized()) {

        console_write(
            "[ FAILED ] Scheduler is not initialized\n"
        );

        return false;
    }

    if (!loader_test_user_stack_collision_retry()) {

        console_write(
            "[ FAILED ] User stack collision/retry test\n"
        );

        return false;
    }

    console_write(
        "[ OK ] User stack collision was rejected and retry succeeded\n"
    );

    if (!loader_test_real_process_launch()) {

        console_write(
            "[ FAILED ] Real ELF user-process launch test\n"
        );

        return false;
    }

    console_write(
        "========== REAL ELF USER-PROCESS LAUNCH TESTS PASSED ==========\n"
    );

    return true;
}
