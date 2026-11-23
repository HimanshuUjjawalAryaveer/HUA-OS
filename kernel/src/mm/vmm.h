
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Opaque virtual address-space object.
 *
 * The implementation owns the PML4 root and tracks
 * references to the address space.
 */
typedef struct vmm_address_space vmm_address_space_t;

/*
 * VMM operation results.
 */
typedef enum {

    VMM_STATUS_OK,

    VMM_STATUS_NOT_INITIALIZED,

    VMM_STATUS_INVALID_ARGUMENT,

    VMM_STATUS_NON_CANONICAL,

    VMM_STATUS_NOT_MAPPED,

    VMM_STATUS_INVALID_PAGE_TABLE,

    VMM_STATUS_ALREADY_MAPPED,

    VMM_STATUS_OUT_OF_MEMORY,

    VMM_STATUS_INVALID_PHYSICAL_ADDRESS,

    VMM_STATUS_UNSUPPORTED

} vmm_status_t;


/*
 * Flags controlling the permissions and cache attributes
 * of a newly created 4 KiB mapping.
 *
 * Supervisor-only, read-only, non-executable is the
 * default when no flags are supplied.
 */
typedef enum {

    VMM_MAP_NONE          = 0,

    VMM_MAP_WRITABLE      = (1ULL << 0),

    VMM_MAP_USER          = (1ULL << 1),

    VMM_MAP_EXECUTABLE    = (1ULL << 2),

    VMM_MAP_GLOBAL        = (1ULL << 3),

    VMM_MAP_WRITE_THROUGH = (1ULL << 4),

    VMM_MAP_CACHE_DISABLE = (1ULL << 5)

} vmm_map_flags_t;


/*
 * Information about a virtual-to-physical mapping.
 */
typedef struct {

    // Physical address corresponding to the queried virtual address.
    uint64_t physical_address;

    // Mapping size: 4 KiB, 2 MiB, or 1 GiB.
    uint64_t page_size;

    // Raw flags from the final mapping entry.
    uint64_t flags;

    // Whether this mapping uses a huge page.
    bool huge_page;

    // Effective permissions after examining all page-table levels.
    bool writable;
    bool user_accessible;
    bool executable;

} vmm_mapping_info_t;


/*
 * Initialize the VMM using the currently active page tables.
 *
 * This does not replace CR3 or discard existing mappings.
 */
struct limine_memmap_response;

bool vmm_init(
    uint64_t hhdm_offset,
    const struct limine_memmap_response *memory_map
);


/*
 * Inspect the mapping of a virtual address.
 */
vmm_status_t vmm_query(
    uint64_t virtual_address,
    vmm_mapping_info_t *mapping
);


/*
 * Return HUA's permanent kernel address space.
 *
 * The kernel address space is owned by VMM and must
 * never be destroyed by callers.
 */
vmm_address_space_t *vmm_kernel_address_space(void);


/*
 * Create an independent address space.
 *
 * The new address space:
 * - Owns a separate PML4 root.
 * - Shares the existing upper-half kernel page-table branches.
 * - Starts with an empty lower-half user address space.
 *
 * Requires the kernel heap to be initialized.
 *
 * CR3 is not changed.
 */
vmm_address_space_t *vmm_address_space_create(void);


/*
 * Acquire another reference to an address space.
 *
 * The kernel address space is permanent and does not
 * require reference counting.
 */
bool vmm_address_space_get(vmm_address_space_t *space);


/*
 * Release one reference to an address space.
 *
 * When the final reference is released, its PML4 root
 * is returned to PMM and its descriptor to the heap.
 *
 * The kernel address space cannot be released.
 */
bool vmm_address_space_put(vmm_address_space_t *space);


/*
 * Query a mapping in a specified address space without
 * switching CR3.
 */
vmm_status_t vmm_address_space_query(
    vmm_address_space_t *space,
    uint64_t virtual_address,
    vmm_mapping_info_t *mapping
);


/*
 * Read the physical address of an address-space PML4 root.
 *
 * This is diagnostic information for tests and later
 * architecture-level context-switch integration.
 */
bool vmm_address_space_get_root_physical(
    const vmm_address_space_t *space,
    uint64_t *physical_address_out
);

/*
 * Activate an address space on a logical CPU.
 *
 * logical_cpu:
 *     The logical CPU executing this function.
 *
 * space:
 *     Registered address space to load into CR3.
 *
 * The operation:
 * - Validates address-space registration and ownership.
 * - Verifies the current hardware CR3.
 * - Switches CR3.
 * - Updates active CPU masks.
 *
 * The caller must execute on the CPU identified by logical_cpu.
 *
 * Current implementation supports CPU0 only.
 */
bool vmm_address_space_activate(
    uint32_t logical_cpu,
    vmm_address_space_t *space
);

vmm_status_t vmm_address_space_map_page(
    vmm_address_space_t *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
);

/*
 * Remove one 4 KiB mapping from an independently owned
 * process address space.
 *
 * physical_address receives the unmapped physical frame.
 * The caller remains responsible for freeing that frame.
 *
 * Only lower-half mappings are permitted.
 */
vmm_status_t vmm_address_space_unmap_page(
    vmm_address_space_t *space,
    uint64_t virtual_address,
    uint64_t *physical_address
);

/*
 * Map one 4 KiB physical frame into the active address space.
 *
 * Both addresses must be page-aligned.
 *
 * The physical frame remains owned by the caller.
 * VMM does not allocate or free the mapped data frame.
 *
 * Existing mappings are never silently replaced.
 */
vmm_status_t vmm_map_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
);


/*
 * Remove one existing 4 KiB mapping.
 *
 * physical_address receives the unmapped physical frame.
 * The caller remains responsible for freeing that frame.
 *
 * Huge-page mappings must be split before using this API.
 */
vmm_status_t vmm_unmap_page(
    uint64_t virtual_address,
    uint64_t *physical_address
);


/*
 * Map a contiguous range of physical 4 KiB frames.
 *
 * page_count specifies the number of pages.
 * The operation must roll back its own changes if it fails.
 *
 * The physical frames remain owned by the caller.
 */
vmm_status_t vmm_map_range(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t page_count,
    uint64_t flags
);


/*
 * Remove a range of 4 KiB mappings.
 *
 * physical_addresses must provide space for page_count
 * physical addresses. Each unmapped physical frame is
 * returned in the corresponding array element.
 *
 * The caller remains responsible for freeing those frames.
 */
vmm_status_t vmm_unmap_range(
    uint64_t virtual_address,
    uint64_t page_count,
    uint64_t *physical_addresses
);

/*
 * Supported protection flags:
 *   VMM_MAP_WRITABLE
 *   VMM_MAP_EXECUTABLE
 *
 * These APIs operate on the kernel address space.
 * VMM_MAP_USER is rejected.
 *
 * User-process permission changes require a future
 * address-space-specific protection API.


 * Change permissions on one existing 4 KiB mapping.
 *
 * Supported flags:
 *   VMM_MAP_WRITABLE
 *   VMM_MAP_USER
 *   VMM_MAP_EXECUTABLE
 *
 * VMM_MAP_NONE means read-only, supervisor-only,
 * and non-executable.
 *
 * W^X is enforced.
 * Existing physical mappings and cache attributes are preserved.
 *
 * Parent page-table permissions are never widened.
 */
vmm_status_t vmm_protect_page(
    uint64_t virtual_address,
    uint64_t flags
);


/*
 * Change permissions on a contiguous range of 4 KiB mappings.
 *
 * All pages must already be mapped as individual 4 KiB pages.
 * Huge-page mappings must be split first.
 *
 * W^X is enforced.
 * Physical mappings and cache attributes are preserved.
 */
vmm_status_t vmm_protect_range(
    uint64_t virtual_address,
    uint64_t page_count,
    uint64_t flags
);


/*
 * Split an existing 2 MiB mapping into 512 individual
 * 4 KiB page mappings.
 *
 * Physical addresses and supported attributes are preserved.
 */
vmm_status_t vmm_split_2m(
    uint64_t virtual_address
);


/*
 * Change the executable permission of an existing 4 KiB mapping.
 *
 * executable = true:  allow instruction execution.
 * executable = false: prevent instruction execution using NX.
 */
vmm_status_t vmm_set_page_executable(
    uint64_t virtual_address,
    bool executable
);


/*
 * CPU NX capability and configuration.
 */
bool vmm_nx_is_supported(void);
bool vmm_nx_is_enabled(void);


/*
 * Existing VMM self-test helper.
 */
bool vmm_test_table_frame_allocation(void);

/*
 * Controlled page-table allocation failure injection.
 *
 * Intended exclusively for VMM testing.
 */
void vmm_test_fail_table_allocation_after(
    uint64_t successful_allocations
);

void vmm_test_clear_table_allocation_failure(void);

/*
 * Controlled process-address-space mapping failure injection.
 *
 * Map failure is triggered after the specified number of
 * successful process-address-space map operations.
 */
void vmm_test_fail_address_space_map_after(
    uint64_t successful_mappings
);

void vmm_test_clear_address_space_map_failure(void);

/*
 * Fail one process-address-space unmap at an exact virtual address.
 */
void vmm_test_fail_address_space_unmap_at(
    uint64_t virtual_address
);

void vmm_test_clear_address_space_unmap_failure(void);

void vmm_test_fail_next_address_space_unmap_once(void);

/*
 * Address-space teardown corruption tests.
 *
 * Injects a duplicate reference to an existing private
 * PML4 child table through an otherwise empty lower-half
 * root entry.
 *
 * The restore function removes only the exact entry
 * installed by the injection function.
 */
bool vmm_test_inject_duplicate_root_entry(
    vmm_address_space_t *space,
    uint64_t *alias_index_out,
    uint64_t *duplicated_entry_out
);

bool vmm_test_restore_duplicate_root_entry(
    vmm_address_space_t *space,
    uint64_t alias_index,
    uint64_t duplicated_entry
);

/*
 * Inject an allocated page-table frame into the ownership
 * registry without linking it into the address-space tree.
 *
 * This deliberately creates an orphan ownership record.
 */
bool vmm_test_inject_orphan_owned_table(
    vmm_address_space_t *space,
    uint64_t *orphan_frame_out
);

/*
 * Remove the injected orphan ownership record and release
 * its physical frame.
 */
bool vmm_test_restore_orphan_owned_table(
    vmm_address_space_t *space,
    uint64_t orphan_frame
);

/*
 * Read one PML4 entry for a registered address space.
 *
 * Supports the permanent kernel address space and
 * independently created process address spaces.
 *
 * Intended exclusively for VMM testing.
 */
bool vmm_test_read_pml4_entry(
    const vmm_address_space_t *space,
    uint64_t index,
    uint64_t *entry_out
);

/*
 * TEST HOOK:
 * Obtain the address space currently recorded for one CPU
 * and verify that its root matches the hardware CR3.
 */
bool vmm_test_get_active_address_space(
    uint32_t logical_cpu,
    vmm_address_space_t **space_out,
    uint64_t *root_physical_out
);

/*
 * TEST HOOK:
 * Read the active CPU bitmask of a registered address space.
 */
bool vmm_test_get_address_space_active_cpu_mask(
    const vmm_address_space_t *space,
    uint64_t *mask_out
);

#endif
