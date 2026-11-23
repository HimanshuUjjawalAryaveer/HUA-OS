#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "sync/raw_spinlock.h"


#define VMM_PAGE_SIZE       4096ULL
#define VMM_PAGE_MASK       (VMM_PAGE_SIZE - 1)

#define VMM_PRESENT         (1ULL << 0)
#define VMM_WRITABLE        (1ULL << 1)
#define VMM_USER            (1ULL << 2)

#define VMM_WRITE_THROUGH   (1ULL << 3)
#define VMM_CACHE_DISABLE   (1ULL << 4)
#define VMM_ACCESSED        (1ULL << 5)
#define VMM_DIRTY           (1ULL << 6)

#define VMM_HUGE_PAGE       (1ULL << 7)
#define VMM_GLOBAL          (1ULL << 8)

#define VMM_HUGE_PAT        (1ULL << 12)
#define VMM_NO_EXECUTE      (1ULL << 63)

#define VMM_SOFTWARE_LOW    (7ULL << 9)
#define VMM_SOFTWARE_HIGH   (0x7FULL << 52)
#define VMM_TABLE_FLAGS     (VMM_PRESENT | VMM_WRITABLE)

#define VMM_EFER_MSR        0xC0000080U
#define VMM_EFER_NXE        (1ULL << 11)

#define VMM_CPUID_EXT_MAX   0x80000000U
#define VMM_CPUID_EXT_NX    0x80000001U
#define VMM_CPUID_NX_BIT    (1U << 20)

#define VMM_PAGE_4K         4096ULL
#define VMM_PAGE_2M         (2ULL * 1024ULL * 1024ULL)
#define VMM_PAGE_1G         (1024ULL * 1024ULL * 1024ULL)

/*
 * Maximum logical CPUs supported by VMM active-space tracking.
 *
 * This currently matches the scheduler's configured limit.
 */
#define VMM_MAX_CPUS 64U


/*
 * Active page-table information.
 */
static uint64_t vmm_hhdm_offset = 0;
static uint64_t vmm_pml4_physical = 0;

static volatile uint64_t *vmm_pml4_virtual = 0;

/*
 * Preserve the low 12 CR3 control bits from the boot CR3.
 *
 * This preserves PWT/PCD when PCID is disabled, or the
 * existing PCID when PCID is enabled.
 *
 * CR3 writes below always clear the no-flush bit by
 * constructing the value from this field and a root PA.
 */
static uint64_t vmm_cr3_low_flags = 0;

static const struct limine_memmap_response *vmm_memory_map = 0;

static bool vmm_initialized = false;

/*
 * Test-only failure injection for process address-space mappings.
 *
 * Map:
 *     Permit N successful address-space mappings, then fail
 *     exactly one subsequent mapping before changing page tables.
 *
 * Unmap:
 *     Fail exactly one unmap of the selected virtual address
 *     before changing its PTE.
 *
 * These hooks are used by the current single-CPU boot tests.
 */
static bool vmm_test_map_failure_enabled = false;
static uint64_t vmm_test_map_successes_remaining = 0;

static bool vmm_test_unmap_failure_enabled = false;
static uint64_t vmm_test_unmap_virtual_address = 0;

static bool vmm_test_unmap_next_failure_enabled = false;

/*
 * Address-space object.
 *
 * Each object owns only its PML4 root at this stage.
 *
 * Its upper-half entries reference shared kernel page-table
 * branches. The kernel branches themselves are not owned by
 * individual process address spaces.
 *
 * Private lower-half page-table allocation will be added
 * with the explicit address-space mapping API.
 */
struct vmm_address_space {

    uint64_t root_physical;

    volatile uint64_t *root_virtual;

    uint64_t reference_count;

    /*
     * Bit N is set when this address space is active
     * on logical CPU N.
     *
     * Protected by vmm_lock.
     */
    uint64_t active_cpu_mask;

    bool is_kernel;

    bool registered;

    struct vmm_address_space *next;
};


/*
 * Protects address-space object lifetime, registry membership,
 * root creation and root destruction.
 *
 * This lock is scheduler-independent, so it also works before
 * scheduler initialization.
 */
static raw_spinlock_t vmm_lock;


/*
 * Permanent kernel address-space object.
 *
 * Its root is the page table originally installed by Limine.
 * VMM never frees this root.
 */
static vmm_address_space_t vmm_kernel_space_object;


/*
 * Registry of independently created address spaces.
 */
static vmm_address_space_t *vmm_address_space_list = NULL;

/*
 * Address space currently loaded in CR3 on each logical CPU.
 *
 * Protected by vmm_lock.
 *
 * NULL means the logical CPU has not been registered
 * with VMM yet.
 */
static vmm_address_space_t *
    vmm_active_address_spaces[VMM_MAX_CPUS] = { NULL };

static bool vmm_nx_supported = false;
static bool vmm_nx_enabled = false;
static uint64_t vmm_get_physical_address_mask(void);
static volatile uint64_t *vmm_get_table(
    uint64_t physical_address
);
static uint8_t vmm_physical_address_bits = 0;


/*
 * One record for each page-table frame allocated by VMM.
 *
 * Ownership is identified by the physical address of the
 * address space's PML4 root.
 */
typedef struct {
    uint64_t physical_address;
    uint64_t owner_root_physical;

    /*
     * Temporary marker used during address-space teardown
     * preflight. Protected by vmm_lock.
     */
    bool validation_visited;

} vmm_owned_table_record_t;

/*
 * Each registry node occupies exactly one PMM frame.
 *
 * Calculate capacity from the actual record and node metadata
 * sizes instead of assuming a fixed number of entries.
 */
#define VMM_OWNED_TABLES_PER_NODE \
    ((VMM_PAGE_SIZE - sizeof(void *) - sizeof(uint64_t)) / \
     sizeof(vmm_owned_table_record_t))

typedef struct vmm_owned_table_node {

    struct vmm_owned_table_node *next;

    uint64_t count;

    vmm_owned_table_record_t records[
        VMM_OWNED_TABLES_PER_NODE
    ];

} vmm_owned_table_node_t;


_Static_assert(
    sizeof(vmm_owned_table_node_t) <= VMM_PAGE_SIZE,
    "VMM ownership registry node exceeds one page"
);


/*
 * First node in the VMM-owned page-table registry.
 */
static vmm_owned_table_node_t *vmm_owned_tables = 0;


/*
 * Test-only allocation failure injection.
 *
 * - Disabled by default.
 * - Can allow a specified number of successful allocations.
 * - Fails the next allocation once.
 * - Automatically disables itself after triggering.
 */
static bool vmm_test_allocation_failure_enabled = false;

static uint64_t vmm_test_allocation_successes_remaining = 0;

/*
 * Tracks the single orphan record injected by the address-space
 * teardown tests.
 *
 * Protected by vmm_lock.
 */
static uint64_t vmm_test_orphan_frame = 0;
static uint64_t vmm_test_orphan_owner_root = 0;

/*
 * Allow a specified number of successful page-table allocations,
 * then fail exactly one allocation.
 */
void vmm_test_fail_table_allocation_after(
    uint64_t successful_allocations
) {
    vmm_test_allocation_successes_remaining =
        successful_allocations;

    vmm_test_allocation_failure_enabled = true;
}


/*
 * Disable allocation failure injection.
 */
void vmm_test_clear_table_allocation_failure(void) {
    vmm_test_allocation_failure_enabled = false;
    vmm_test_allocation_successes_remaining = 0;
}

/*
 * Fail one process-address-space map after N successful maps.
 *
 * The failure occurs before the selected mapping modifies
 * any page-table entry.
 */
void vmm_test_fail_address_space_map_after(
    uint64_t successful_mappings
) {
    vmm_test_map_successes_remaining =
        successful_mappings;

    vmm_test_map_failure_enabled = true;
}


/*
 * Clear process-address-space map failure injection.
 */
void vmm_test_clear_address_space_map_failure(void) {
    vmm_test_map_failure_enabled = false;
    vmm_test_map_successes_remaining = 0;
}


/*
 * Fail exactly one process-address-space unmap at a selected VA.
 *
 * The failure occurs before the selected PTE is modified.
 */
void vmm_test_fail_address_space_unmap_at(
    uint64_t virtual_address
) {
    vmm_test_unmap_next_failure_enabled = false;

    vmm_test_unmap_virtual_address =
        virtual_address;

    vmm_test_unmap_failure_enabled = true;
}


/*
 * Clear process-address-space unmap failure injection.
 */
void vmm_test_clear_address_space_unmap_failure(void) {
    vmm_test_unmap_failure_enabled = false;
    vmm_test_unmap_virtual_address = 0;
    vmm_test_unmap_next_failure_enabled = false;
}

/*
 * Fail the next process-address-space unmap operation.
 *
 * Unlike the exact-address hook, this does not depend
 * on which virtual address is being unmapped.
 */
void vmm_test_fail_next_address_space_unmap_once(void) {

    vmm_test_unmap_failure_enabled = false;
    vmm_test_unmap_virtual_address = 0;

    vmm_test_unmap_next_failure_enabled = true;
}

/*
 * Allocate one physical frame for a new page table.
 *
 * Returns its HHDM virtual address and stores its physical
 * address in physical_out.
 */
static volatile uint64_t *vmm_allocate_table_frame(
    uint64_t *physical_out
) {

    if (!vmm_initialized || physical_out == 0) {
        return 0;
    }

    /*
     * Apply controlled allocation failure injection.
     *
     * When the countdown reaches zero, fail this allocation
     * once and automatically disable the test hook.
     */
    if (vmm_test_allocation_failure_enabled &&
        vmm_test_allocation_successes_remaining == 0) {

        vmm_test_allocation_failure_enabled = false;

        return 0;
    }

    // Request one zeroed physical frame from PMM.
    uint64_t physical_address = pmm_alloc_frame();

    /*
     * Decrement the countdown only when PMM successfully
     * supplies a physical frame.
     */
    if (physical_address != 0 &&
        vmm_test_allocation_failure_enabled) {

        vmm_test_allocation_successes_remaining--;
    }

    if (physical_address == 0) {
        return 0;
    }

    // Obtain a virtual pointer through the HHDM.
    void *virtual_address =
        pmm_frame_to_virt(physical_address);

    if (virtual_address == 0 ||
        ((uintptr_t)virtual_address & VMM_PAGE_MASK) != 0) {

        pmm_free_frame(physical_address);
        return 0;
    }

    // Return both addresses to the caller.
    *physical_out = physical_address;

    return (volatile uint64_t *)virtual_address;
}


/*
 * Find a registered page-table frame and retrieve its owner.
 *
 * Returns false if the physical frame is not registered.
 *
 * The VMM lock must be held by the caller.
 */
static bool vmm_find_owned_table(
    uint64_t physical_address,
    uint64_t *owner_root_physical_out
) {
    if (physical_address == 0 ||
        (physical_address & VMM_PAGE_MASK) != 0) {
        return false;
    }

    for (vmm_owned_table_node_t *node = vmm_owned_tables;
         node != 0;
         node = node->next) {

        for (uint64_t i = 0; i < node->count; i++) {

            if (node->records[i].physical_address !=
                physical_address) {
                continue;
            }

            if (owner_root_physical_out != 0) {
                *owner_root_physical_out =
                    node->records[i].owner_root_physical;
            }

            return true;
        }
    }

    return false;
}

/*
 * Mark an owned page-table frame as visited during address-space
 * teardown validation.
 *
 * The caller must hold vmm_lock.
 *
 * Returns false if:
 * - The frame is not registered.
 * - The frame belongs to another address space.
 * - The frame has already been visited.
 */
static bool vmm_mark_owned_table_visited(
    uint64_t physical_address,
    uint64_t owner_root_physical
) {
    for (
        vmm_owned_table_node_t *node = vmm_owned_tables;
        node != NULL;
        node = node->next
    ) {
        for (uint64_t i = 0; i < node->count; i++) {
            vmm_owned_table_record_t *record =
                &node->records[i];

            if (record->physical_address != physical_address) {
                continue;
            }

            if (
                record->owner_root_physical != owner_root_physical ||
                record->validation_visited
            ) {
                return false;
            }

            record->validation_visited = true;

            return true;
        }
    }

    return false;
}

/*
 * Register a physical frame as a VMM-owned page table.
 *
 * Every record identifies both the table frame and the
 * address-space root that owns it.
 *
 * The VMM lock must be held by the caller.
 */
static bool vmm_register_owned_table(
    uint64_t physical_address,
    uint64_t owner_root_physical
) {
    if (!vmm_initialized ||
        physical_address == 0 ||
        owner_root_physical == 0 ||
        (physical_address & VMM_PAGE_MASK) != 0 ||
        (owner_root_physical & VMM_PAGE_MASK) != 0 ||
        vmm_find_owned_table(physical_address, 0)) {

        return false;
    }

    /*
     * Reuse an existing node if it has free capacity.
     */
    for (vmm_owned_table_node_t *node = vmm_owned_tables;
         node != 0;
         node = node->next) {

        if (node->count < VMM_OWNED_TABLES_PER_NODE) {

            node->records[node->count].physical_address =
                physical_address;

            node->records[node->count].owner_root_physical =
                owner_root_physical;

            node->records[node->count].validation_visited = false;

            node->count++;

            return true;
        }
    }

    /*
     * No existing node has capacity.
     * Allocate one PMM frame for another registry node.
     */
    uint64_t node_physical = pmm_alloc_frame();

    if (node_physical == 0) {
        return false;
    }

    vmm_owned_table_node_t *new_node =
        (vmm_owned_table_node_t *)
            pmm_frame_to_virt(node_physical);

    if (new_node == 0) {
        pmm_free_frame(node_physical);
        return false;
    }

    /*
     * PMM zeroes newly allocated frames.
     * Initialize the new node and its first record.
     */
    new_node->count = 1;

    new_node->records[0].physical_address =
        physical_address;

    new_node->records[0].owner_root_physical =
        owner_root_physical;

    new_node->records[0].validation_visited = false;

    new_node->next = vmm_owned_tables;

    vmm_owned_tables = new_node;

    return true;
}


/*
 * Remove a page-table ownership record.
 *
 * The physical frame itself is NOT freed here.
 *
 * Removal requires both the physical address and matching
 * owner root. This prevents one address space from unregistering
 * another address space's page-table frame.
 *
 * The VMM lock must be held by the caller.
 */
static bool vmm_unregister_owned_table(
    uint64_t physical_address,
    uint64_t owner_root_physical
) {
    if (physical_address == 0 ||
        owner_root_physical == 0 ||
        (physical_address & VMM_PAGE_MASK) != 0 ||
        (owner_root_physical & VMM_PAGE_MASK) != 0) {
        return false;
    }

    for (vmm_owned_table_node_t *node = vmm_owned_tables;
         node != 0;
         node = node->next) {

        for (uint64_t i = 0; i < node->count; i++) {

            if (node->records[i].physical_address !=
                    physical_address ||
                node->records[i].owner_root_physical !=
                    owner_root_physical) {
                continue;
            }

            /*
             * Fill the removed slot with the last record.
             */
            node->count--;

            node->records[i] =
                node->records[node->count];

            node->records[node->count].physical_address = 0;
            node->records[node->count].owner_root_physical = 0;
            node->records[node->count].validation_visited = false;

            return true;
        }
    }

    return false;
}

/*
 * Release an already-detached, VMM-owned page-table frame.
 *
 * The caller must hold vmm_lock and must already have:
 * - Verified that the table is empty.
 * - Verified that the table belongs to owner_root_physical.
 * - Detached the table from parent_entry.
 *
 * Transactional behaviour:
 *
 * - On success, the ownership record is removed and the
 *   physical frame is released.
 *
 * - If PMM refuses to free the frame, its ownership record
 *   is restored and the original parent entry is reconnected.
 *
 * This makes a transient PMM free failure retryable.
 */
static bool vmm_release_detached_owned_table_locked(
    volatile uint64_t *parent_entry,
    uint64_t saved_parent_entry,
    uint64_t owner_root_physical,
    uint64_t table_physical
) {
    if (parent_entry == NULL ||
        saved_parent_entry == 0 ||
        !(saved_parent_entry & VMM_PRESENT) ||
        owner_root_physical == 0 ||
        table_physical == 0 ||
        (owner_root_physical & VMM_PAGE_MASK) != 0 ||
        (table_physical & VMM_PAGE_MASK) != 0) {

        return false;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0 ||
        (saved_parent_entry & address_mask) != table_physical) {

        return false;
    }

    /*
     * The caller must already have detached this exact entry.
     * Never overwrite an unexpected parent entry.
     */
    if (__atomic_load_n(
            parent_entry,
            __ATOMIC_ACQUIRE
        ) != 0) {

        return false;
    }

    /*
     * Verify the ownership record before changing it.
     */
    uint64_t recorded_owner = 0;

    if (!vmm_find_owned_table(
            table_physical,
            &recorded_owner
        ) ||
        recorded_owner != owner_root_physical) {

        return false;
    }

    /*
     * Remove the ownership record before freeing the frame.
     */
    if (!vmm_unregister_owned_table(
            table_physical,
            owner_root_physical
        )) {

        /*
         * The frame has not been freed.
         * Attempt to restore its original parent reference.
         */
        uint64_t expected_entry = 0;

        __atomic_compare_exchange_n(
            parent_entry,
            &expected_entry,
            saved_parent_entry,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE
        );

        return false;
    }

    /*
     * Attempt to release the physical frame.
     */
    if (pmm_free_frame(table_physical)) {
        return true;
    }

    /*
     * PMM failure must not be treated as proof that the
     * physical frame is still allocated.
     *
     * Confirm that the frame remains backed by an allocation
     * before restoring it into the page-table hierarchy.
     */
    if (pmm_frame_to_virt(table_physical) == NULL) {
        return false;
    }

    /*
     * Restore ownership first.
     *
     * unregister just removed this exact record, so its
     * registry node has an available slot for reinsertion.
     */
    if (!vmm_register_owned_table(
            table_physical,
            owner_root_physical
        )) {

        return false;
    }

    /*
     * Reconnect the original parent entry.
     *
     * The VMM lock prevents another VMM operation from
     * modifying this parent entry during the transaction.
     */
    uint64_t expected_entry = 0;

    if (!__atomic_compare_exchange_n(
            parent_entry,
            &expected_entry,
            saved_parent_entry,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE
        )) {

        /*
         * Keep the ownership record if reconnection fails.
         * This leaves the frame tracked rather than freeing
         * or overwriting an unexpected page-table entry.
         */
        return false;
    }

    /*
     * The caller will report failure. The original empty
     * table and its ownership are now restored for retry.
     */
    return false;
}

/*
 * Obtain an existing intermediate page table or create a new one.
 *
 * parent_entry:
 *     Entry in the parent page table.
 *
 * map_flags:
 *     Required permissions for the eventual mapping.
 *
 * table_out:
 *     Receives the virtual address of the child table.
 *
 * created_out:
 *     Receives true if this function created the child table.
 *
 * Newly created page tables are registered as VMM-owned.
 */
static vmm_status_t vmm_get_or_create_table(
    volatile uint64_t *parent_entry,
    uint64_t map_flags,
    bool process_mapping,
    uint64_t owner_root_physical,
    volatile uint64_t **table_out,
    bool *created_out
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (parent_entry == 0 ||
        owner_root_physical == 0 ||
        (owner_root_physical & VMM_PAGE_MASK) != 0 ||
        table_out == 0 ||
        created_out == 0 ||
        ((uintptr_t)parent_entry & 7ULL) != 0) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    *table_out = 0;
    *created_out = false;

    const uint64_t allowed_flags =
        VMM_MAP_WRITABLE |
        VMM_MAP_USER |
        VMM_MAP_EXECUTABLE |
        VMM_MAP_GLOBAL |
        VMM_MAP_WRITE_THROUGH |
        VMM_MAP_CACHE_DISABLE;

    if (map_flags & ~allowed_flags) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Read the parent entry atomically.
     */
    uint64_t entry =
        __atomic_load_n(parent_entry, __ATOMIC_ACQUIRE);

    /*
     * An existing intermediate table is already present.
     */
    if (entry & VMM_PRESENT) {

        /*
         * A huge-page entry cannot be used as an intermediate
         * page-table pointer.
         */
        if (entry & VMM_HUGE_PAGE) {
            return VMM_STATUS_INVALID_PAGE_TABLE;
        }

        /*
         * Reject physical-address bits unsupported by the CPU.
         */
        uint64_t physical_field_mask =
            ((1ULL << 52) - 1ULL) & ~VMM_PAGE_MASK;

        if (entry & physical_field_mask & ~address_mask) {
            return VMM_STATUS_INVALID_PAGE_TABLE;
        }

        /*
         * Existing ancestor permissions must not prevent the
         * requested mapping from working.
         *
         * We deliberately do not widen existing permissions.
         */
        if ((map_flags & VMM_MAP_WRITABLE) &&
            !(entry & VMM_WRITABLE)) {

            return VMM_STATUS_INVALID_ARGUMENT;
        }

        /*
        * Every intermediate entry in a process address space
        * must permit user traversal.
        *
        * The leaf PTE independently determines whether the
        * actual page is accessible from Ring 3.
        *
        * Never widen an existing supervisor-only ancestor.
        */
        if (process_mapping &&
            !(entry & VMM_USER)) {

            return VMM_STATUS_INVALID_ARGUMENT;
        }

        if ((map_flags & VMM_MAP_EXECUTABLE) &&
            vmm_nx_enabled &&
            (entry & VMM_NO_EXECUTE)) {

            return VMM_STATUS_INVALID_ARGUMENT;
        }

        uint64_t table_physical =
            entry & address_mask;

        volatile uint64_t *table =
            vmm_get_table(table_physical);

        if (table == 0) {
            return VMM_STATUS_INVALID_PAGE_TABLE;
        }

        *table_out = table;

        return VMM_STATUS_OK;
    }

    /*
     * A non-present entry should be completely zero.
     * Do not overwrite unexpected software or reserved bits.
     */
    if (entry != 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Allocate a zeroed physical frame for the new table.
     */
    uint64_t table_physical = 0;

    volatile uint64_t *new_table =
        vmm_allocate_table_frame(&table_physical);

    if (new_table == 0) {
        return VMM_STATUS_OUT_OF_MEMORY;
    }

    /*
     * Validate the allocated frame.
     */
    if ((table_physical & VMM_PAGE_MASK) != 0 ||
        (table_physical & ~address_mask) != 0) {

        pmm_free_frame(table_physical);

        return VMM_STATUS_INVALID_PHYSICAL_ADDRESS;
    }

    /*
     * Register ownership before making the table reachable
     * through the active page-table hierarchy.
     */
    if (!vmm_register_owned_table(
            table_physical,
            owner_root_physical
        )) {

        pmm_free_frame(table_physical);

        return VMM_STATUS_OUT_OF_MEMORY;
    }

    /*
     * Intermediate tables are permissive.
     *
     * The final leaf PTE will enforce the requested permissions.
     * Existing ancestor restrictions are never widened.
     */
    uint64_t new_entry =
        (table_physical & address_mask) |
        VMM_PRESENT |
        VMM_WRITABLE;

    /*
    * Process-address-space intermediate tables must permit
    * user traversal for both user and supervisor-only leaves.
    *
    * Kernel-root intermediate tables remain supervisor-only.
    */
    if (process_mapping) {
        new_entry |= VMM_USER;
    }

    /*
     * Publish the new table only if the parent entry is still
     * empty. This prevents silently replacing another entry.
     */
    uint64_t expected_entry = 0;

    if (!__atomic_compare_exchange_n(
            parent_entry,
            &expected_entry,
            new_entry,
            false,
            __ATOMIC_RELEASE,
            __ATOMIC_ACQUIRE)) {

        /*
         * The table was not published. Remove its ownership
         * record before returning its frame to PMM.
         */
        if (!vmm_unregister_owned_table(
                table_physical,
                owner_root_physical
            )) {
            /*
             * Preserve the frame if ownership bookkeeping
             * unexpectedly fails. Avoid freeing an untracked
             * or potentially referenced frame.
             */
            return VMM_STATUS_INVALID_PAGE_TABLE;
        }

        pmm_free_frame(table_physical);

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    *table_out = new_table;
    *created_out = true;

    return VMM_STATUS_OK;
}

/*
 * Populate a new page table with 512 individual mappings
 * equivalent to an existing 2 MiB huge-page mapping.
 *
 * This function does not modify the active page directory.
 */
/*
 * Populate a new page table with 512 individual mappings
 * equivalent to an existing 2 MiB huge-page mapping.
 *
 * This function does not modify the active page directory.
 */
static bool vmm_split_2m_mapping(
    uint64_t huge_entry,
    volatile uint64_t *new_table
) {
    if (new_table == 0) {
        return false;
    }

    // The original entry must be present and use a 2 MiB page.
    if (!(huge_entry & VMM_PRESENT) ||
        !(huge_entry & VMM_HUGE_PAGE)) {
        return false;
    }

    // Bits 13-20 are reserved in a 2 MiB page-directory entry.
    if (huge_entry & (0xFFULL << 13)) {
        return false;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return false;
    }

    // Extract the aligned 2 MiB physical base.
    uint64_t physical_base =
        huge_entry & address_mask & ~(VMM_PAGE_2M - 1ULL);

    // Ensure the complete 2 MiB region is physically addressable.
    uint64_t maximum_physical_address =
        address_mask | VMM_PAGE_MASK;

    if (physical_base >
        maximum_physical_address - (VMM_PAGE_2M - 1ULL)) {
        return false;
    }

    // Preserve common mapping attributes.
    uint64_t flags = huge_entry &
        (VMM_PRESENT |
         VMM_WRITABLE |
         VMM_USER |
         VMM_WRITE_THROUGH |
         VMM_CACHE_DISABLE |
         VMM_ACCESSED |
         VMM_DIRTY |
         VMM_GLOBAL |
         VMM_NO_EXECUTE);

    // Preserve software-available bits 9-11 and 52-58.
    flags |= huge_entry &
        ((7ULL << 9) | (0x7FULL << 52));

    // Translate huge-page PAT bit 12 into 4 KiB PTE PAT bit 7.
    if (huge_entry & VMM_HUGE_PAT) {
        flags |= VMM_HUGE_PAGE;
    }

    // Generate 512 consecutive 4 KiB mappings.
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t physical_address =
            physical_base + (i * VMM_PAGE_SIZE);

        new_table[i] = physical_address | flags;
    }

    return true;
}

/*
 * Construct a page-directory entry pointing to a new page table.
 *
 * The original 2 MiB mapping's permissions are preserved.
 * Individual 4 KiB entries control their own NX permissions.
 */
static uint64_t vmm_create_table_entry(
    uint64_t original_entry,
    uint64_t table_physical
) {
    uint64_t flags = VMM_PRESENT;

    // Preserve the original writable permission.
    if (original_entry & VMM_WRITABLE) {
        flags |= VMM_WRITABLE;
    }

    // Preserve the original user-accessible permission.
    if (original_entry & VMM_USER) {
        flags |= VMM_USER;
    }

    // Preserve software-available bits.
    flags |= original_entry &
        (VMM_SOFTWARE_LOW | VMM_SOFTWARE_HIGH);

    // The new entry points to a page table, not a huge page.
    // NX is deliberately cleared at this level.
    return (table_physical & vmm_get_physical_address_mask()) |
           flags;
}

/*
 * Invalidate one virtual address from the CPU's TLB.
 */
static void vmm_invalidate_page(uint64_t virtual_address) {

    asm volatile (
        "invlpg (%0)"
        :
        : "r"(virtual_address)
        : "memory"
    );
}

/*
 * Invalidate every 4 KiB translation within a 2 MiB region.
 *
 * This ensures stale translations from a previous huge-page
 * mapping cannot remain cached after the page-table split.
 */
static void vmm_invalidate_2m_range(uint64_t virtual_address) {

    uint64_t base =
        virtual_address & ~(VMM_PAGE_2M - 1ULL);

    for (uint64_t offset = 0;
         offset < VMM_PAGE_2M;
         offset += VMM_PAGE_SIZE) {

        vmm_invalidate_page(base + offset);
    }
}

/*
 * Test page-table frame allocation, zero initialization,
 * and safe release.
 */
bool vmm_test_table_frame_allocation(void) {

    uint64_t free_before = pmm_get_free_frames();

    uint64_t physical_address = 0;

    volatile uint64_t *table =
        vmm_allocate_table_frame(&physical_address);

    if (table == 0) {
        return false;
    }

    // Verify physical address alignment.
    if (physical_address & VMM_PAGE_MASK) {
        pmm_free_frame(physical_address);
        return false;
    }

    // Verify all 512 entries are zero.
    for (uint64_t i = 0; i < 512; i++) {

        if (table[i] != 0) {
            pmm_free_frame(physical_address);
            return false;
        }
    }

    // Release the allocated frame.
    if (!pmm_free_frame(physical_address)) {
        return false;
    }

    // Confirm that PMM accounting is restored.
    if (pmm_get_free_frames() != free_before) {
        return false;
    }

    return true;
}

/*
 * Detect the maximum physical-address width supported by the CPU.
 */
static bool vmm_detect_physical_address_width(void) {

    uint32_t eax = 0x80000000U;
    uint32_t ebx;
    uint32_t ecx = 0;
    uint32_t edx;

    asm volatile (
        "cpuid"
        : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
    );

    // Extended CPUID leaf 0x80000008 must be supported.
    if (eax < 0x80000008U) {
        return false;
    }

    eax = 0x80000008U;
    ecx = 0;

    asm volatile (
        "cpuid"
        : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
    );

    uint8_t physical_bits = (uint8_t)(eax & 0xFFU);

    // Four-level x86-64 paging requires at least 36 bits.
    if (physical_bits < 36 || physical_bits > 52) {
        return false;
    }

    vmm_physical_address_bits = physical_bits;

    return true;
}

/*
 * Return the physical-address mask supported by the CPU.
 */
static uint64_t vmm_get_physical_address_mask(void) {

    if (vmm_physical_address_bits == 0) {
        return 0;
    }

    return
        ((1ULL << vmm_physical_address_bits) - 1ULL)
        & ~VMM_PAGE_MASK;
}

static void vmm_detect_nx(void) {

    vmm_nx_supported = false;
    vmm_nx_enabled = false;

    uint32_t eax = VMM_CPUID_EXT_MAX;
    uint32_t ebx;
    uint32_t ecx = 0;
    uint32_t edx;

    // Find the highest supported extended CPUID leaf.
    asm volatile (
        "cpuid"
        : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
    );

    if (eax < VMM_CPUID_EXT_NX) {
        return;
    }

    // Check the NX feature bit.
    eax = VMM_CPUID_EXT_NX;
    ecx = 0;

    asm volatile (
        "cpuid"
        : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
    );

    if (!(edx & VMM_CPUID_NX_BIT)) {
        return;
    }

    vmm_nx_supported = true;

    // Read the Extended Feature Enable Register (EFER).
    uint32_t efer_low;
    uint32_t efer_high;

    asm volatile (
        "rdmsr"
        : "=a"(efer_low), "=d"(efer_high)
        : "c"(VMM_EFER_MSR)
    );

    uint64_t efer =
        ((uint64_t)efer_high << 32) | efer_low;

    vmm_nx_enabled = (efer & VMM_EFER_NXE) != 0;
}

// This helper combines permissions from one page-table entry with the permissions already accumulated from previous levels.
static void vmm_set_effective_permissions(
    vmm_mapping_info_t *mapping,
    uint64_t entry_flags
) {
    mapping->writable =
        mapping->writable &&
        ((entry_flags & VMM_WRITABLE) != 0);

    mapping->user_accessible =
        mapping->user_accessible &&
        ((entry_flags & VMM_USER) != 0);

    if (vmm_nx_enabled) {
        mapping->executable =
            mapping->executable &&
            ((entry_flags & VMM_NO_EXECUTE) == 0);
    }
}

/*
 * Check whether an address is canonical for four-level paging.
 */
static bool vmm_is_canonical(uint64_t address) {

    uint64_t upper = address >> 48;

    if (address & (1ULL << 47)) {
        return upper == 0xFFFF;
    }

    return upper == 0;
}


/*
 * Check whether a memory-map type is mapped by Limine's HHDM
 * under base revision 6.
 */
static bool vmm_hhdm_type_is_mapped(uint64_t type) {

    return
        type == LIMINE_MEMMAP_USABLE ||
        type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
        type == LIMINE_MEMMAP_ACPI_NVS ||
        type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
        type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES ||
        type == LIMINE_MEMMAP_FRAMEBUFFER ||
        type == LIMINE_MEMMAP_RESERVED_MAPPED;
}


/*
 * Verify that a complete physical page belongs to an HHDM-mapped
 * memory-map region.
 */
static bool vmm_physical_page_is_mapped(
    uint64_t physical_address
) {

    if (vmm_memory_map == 0 ||
        vmm_memory_map->entries == 0 ||
        vmm_memory_map->entry_count == 0) {

        return false;
    }

    if (physical_address & VMM_PAGE_MASK) {
        return false;
    }

    if (physical_address > UINT64_MAX - VMM_PAGE_SIZE) {
        return false;
    }

    uint64_t page_end = physical_address + VMM_PAGE_SIZE;

    for (uint64_t i = 0;
         i < vmm_memory_map->entry_count;
         i++) {

        struct limine_memmap_entry *entry =
            vmm_memory_map->entries[i];

        if (entry == 0 ||
            entry->length < VMM_PAGE_SIZE) {
            continue;
        }

        if (entry->base > UINT64_MAX - entry->length) {
            continue;
        }

        uint64_t region_end =
            entry->base + entry->length;

        if (physical_address >= entry->base &&
            page_end <= region_end &&
            vmm_hhdm_type_is_mapped(entry->type)) {

            return true;
        }
    }

    return false;
}


/*
 * Convert a validated physical page-table address into an HHDM
 * virtual pointer.
 *
 * This does not require the frame to be allocated by the PMM.
 */
static volatile uint64_t *vmm_get_table(
    uint64_t physical_address
) {

    if (!vmm_physical_page_is_mapped(physical_address)) {
        return 0;
    }

    if (vmm_hhdm_offset >
        UINT64_MAX - physical_address) {

        return 0;
    }

    uint64_t virtual_address =
        vmm_hhdm_offset + physical_address;

    if (!vmm_is_canonical(virtual_address)) {
        return 0;
    }

    return (volatile uint64_t *)(uintptr_t)virtual_address;
}


/*
 * Initialize read-only access to the active page tables.
 *
 * No page tables are allocated or modified here.
 */
bool vmm_init(
    uint64_t hhdm_offset,
    const struct limine_memmap_response *memory_map
) {

    vmm_initialized = false;
    vmm_hhdm_offset = 0;
    vmm_pml4_physical = 0;
    vmm_pml4_virtual = 0;
    vmm_cr3_low_flags = 0;
    vmm_memory_map = 0;
    vmm_nx_supported = false;
    vmm_nx_enabled = false;

    if (hhdm_offset == 0 ||
        memory_map == 0 ||
        memory_map->entries == 0 ||
        memory_map->entry_count == 0) {

        return false;
    }

    vmm_detect_nx();
    if (!vmm_detect_physical_address_width()) {
        return false;
    }

    vmm_hhdm_offset = hhdm_offset;
    vmm_memory_map = memory_map;

    uint64_t cr3;

    asm volatile (
        "mov %%cr3, %0"
        : "=r"(cr3)
        :
        : "memory"
    );

    /*
    * Retain the CR3 low control bits.
    *
    * The physical root address is extracted separately.
    */
    vmm_cr3_low_flags = cr3 & VMM_PAGE_MASK;

    uint64_t pml4_physical =
    cr3 & vmm_get_physical_address_mask();

    if (!vmm_physical_page_is_mapped(pml4_physical)) {
        return false;
    }

    volatile uint64_t *pml4 =
        vmm_get_table(pml4_physical);

    if (pml4 == 0) {
        return false;
    }

    vmm_pml4_physical = pml4_physical;
    vmm_pml4_virtual = pml4;


    /*
    * Initialize address-space synchronization.
    */
    if (!raw_spinlock_init(&vmm_lock)) {

        vmm_pml4_physical = 0;
        vmm_pml4_virtual = NULL;

        return false;
    }


    /*
    * No independent address spaces exist during initial VMM
    * initialization.
    */
    vmm_address_space_list = NULL;

    /*
    * Initialize per-CPU address-space tracking.
    *
    * At boot, CPU0 is executing with the Limine-provided
    * kernel page-table root.
    */
    for (uint32_t i = 0; i < VMM_MAX_CPUS; i++) {
        vmm_active_address_spaces[i] = NULL;
    }

    /*
    * Register the permanent kernel address-space object.
    *
    * Its PML4 root is the active Limine-created root.
    * It is not allocated by PMM and must never be freed.
    */
    vmm_kernel_space_object.root_physical =
        vmm_pml4_physical;

    vmm_kernel_space_object.root_virtual =
        vmm_pml4_virtual;

    vmm_kernel_space_object.reference_count =
        UINT64_MAX;

    /*
    * The initial kernel CR3 is active on CPU0.
    */
    vmm_kernel_space_object.active_cpu_mask = 1ULL;

    vmm_active_address_spaces[0] =
        &vmm_kernel_space_object;

    vmm_kernel_space_object.is_kernel = true;

    vmm_kernel_space_object.registered = true;

    vmm_kernel_space_object.next = NULL;


    vmm_initialized = true;

    return true;
}


/*
 * Query the physical mapping of a virtual address.
 *
 * Supports:
 * - 4 KiB pages
 * - 2 MiB huge pages
 * - 1 GiB huge pages
 *
 * This function only reads the active page tables.
 */
static vmm_status_t vmm_query_root(
    volatile uint64_t *root,
    uint64_t virtual_address,
    vmm_mapping_info_t *mapping
) {

    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (root == NULL || mapping == NULL) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    mapping->physical_address = 0;
    mapping->page_size = 0;
    mapping->flags = 0;
    mapping->huge_page = false;
    mapping->writable = true;
    mapping->user_accessible = true;
    mapping->executable = true;

    /*
     * Extract the four page-table indexes.
     */
    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FFULL;

    uint64_t pdpt_index =
        (virtual_address >> 30) & 0x1FFULL;

    uint64_t pd_index =
        (virtual_address >> 21) & 0x1FFULL;

    uint64_t pt_index =
        (virtual_address >> 12) & 0x1FFULL;


    /*
     * Level 4: PML4.
     */
    uint64_t pml4_entry =
        __atomic_load_n(
            &root[pml4_index],
            __ATOMIC_ACQUIRE
        );

    vmm_set_effective_permissions(mapping, pml4_entry);

    if (!(pml4_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    // PS is reserved in a PML4 entry.
    if (pml4_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    uint64_t pdpt_physical =
        pml4_entry & vmm_get_physical_address_mask();

    volatile uint64_t *pdpt =
        vmm_get_table(pdpt_physical);

    if (pdpt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }


    /*
     * Level 3: PDPT.
     */
    uint64_t pdpt_entry =
        pdpt[pdpt_index];

    vmm_set_effective_permissions(mapping, pdpt_entry);

    if (!(pdpt_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    /*
     * A PDPT entry with PS=1 maps a 1 GiB page directly.
     */
    if (pdpt_entry & VMM_HUGE_PAGE) {

        uint64_t physical_base =
            pdpt_entry &
                (vmm_get_physical_address_mask() &
                ~(VMM_PAGE_1G - 1ULL));

        uint64_t offset =
            virtual_address & (VMM_PAGE_1G - 1);

        mapping->physical_address =
            physical_base | offset;

        mapping->page_size = VMM_PAGE_1G;
        mapping->flags = pdpt_entry;
        mapping->huge_page = true;

        return VMM_STATUS_OK;
    }

    uint64_t pd_physical =
        pdpt_entry & vmm_get_physical_address_mask();

    volatile uint64_t *pd =
        vmm_get_table(pd_physical);

    if (pd == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }


    /*
     * Level 2: Page Directory.
     */
    uint64_t pd_entry =
        pd[pd_index];

    vmm_set_effective_permissions(mapping, pd_entry);

    if (!(pd_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    /*
     * A PD entry with PS=1 maps a 2 MiB page directly.
     */
    if (pd_entry & VMM_HUGE_PAGE) {

        uint64_t physical_base =
            pd_entry &
                (vmm_get_physical_address_mask() &
                ~(VMM_PAGE_2M - 1ULL));

        uint64_t offset =
            virtual_address & (VMM_PAGE_2M - 1);

        mapping->physical_address =
            physical_base | offset;

        mapping->page_size = VMM_PAGE_2M;
        mapping->flags = pd_entry;
        mapping->huge_page = true;

        return VMM_STATUS_OK;
    }

    uint64_t pt_physical =
        pd_entry & vmm_get_physical_address_mask();

    volatile uint64_t *pt =
        vmm_get_table(pt_physical);

    if (pt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }


    /*
     * Level 1: Page Table.
     */
    uint64_t pt_entry =
        pt[pt_index];

    vmm_set_effective_permissions(mapping, pt_entry);

    if (!(pt_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    uint64_t physical_base =
        pt_entry & vmm_get_physical_address_mask();

    uint64_t offset =
        virtual_address & (VMM_PAGE_4K - 1);

    mapping->physical_address =
        physical_base | offset;

    mapping->page_size = VMM_PAGE_4K;
    mapping->flags = pt_entry;
    mapping->huge_page = false;

    return VMM_STATUS_OK;
}


vmm_status_t vmm_query(
    uint64_t virtual_address,
    vmm_mapping_info_t *mapping
) {

    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    vmm_status_t status = vmm_query_root(
        vmm_pml4_virtual,
        virtual_address,
        mapping
    );

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}


/*
 * Check whether an address-space object is registered.
 *
 * Caller must hold vmm_lock.
 */
static bool vmm_address_space_is_registered_unlocked(
    const vmm_address_space_t *space
) {

    if (space == NULL) {
        return false;
    }

    if (space == &vmm_kernel_space_object) {
        return true;
    }

    for (
        vmm_address_space_t *current = vmm_address_space_list;
        current != NULL;
        current = current->next
    ) {

        if (current == space && current->registered) {
            return true;
        }
    }

    return false;
}

/*
 * TEST HOOK:
 * Read one PML4 entry from a registered address space.
 *
 * This deliberately uses the VMM's trusted root pointer
 * instead of pmm_frame_to_virt().
 *
 * The kernel root may originate from Limine and need not
 * be marked as a PMM-allocated frame.
 */
bool vmm_test_read_pml4_entry(
    const vmm_address_space_t *space,
    uint64_t index,
    uint64_t *entry_out
) {
    if (!vmm_initialized ||
        space == NULL ||
        index >= 512 ||
        entry_out == NULL) {

        return false;
    }

    *entry_out = 0;

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    bool result = false;

    if (vmm_address_space_is_registered_unlocked(space)) {

        volatile uint64_t *root = NULL;

        if (space == &vmm_kernel_space_object) {

            root = vmm_pml4_virtual;

        } else if (
            space->registered &&
            !space->is_kernel &&
            space->root_physical != 0 &&
            space->root_virtual != NULL &&
            (space->root_physical & VMM_PAGE_MASK) == 0 &&
            ((uintptr_t)space->root_virtual & VMM_PAGE_MASK) == 0
        ) {

            root = space->root_virtual;
        }

        if (root != NULL) {

            *entry_out =
                __atomic_load_n(
                    &root[index],
                    __ATOMIC_ACQUIRE
                );

            result = true;
        }
    }

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}

/*
 * Validate that one kernel upper-half PML4 entry is correctly
 * mirrored in every independently created address space.
 *
 * The caller must hold vmm_lock.
 *
 * This is a preflight operation: it must complete before
 * a kernel mapping operation changes the kernel root.
 */
static bool vmm_validate_kernel_pml4_mirror_locked(
    uint64_t pml4_index
) {
    if (pml4_index < 256 ||
        pml4_index >= 512 ||
        vmm_pml4_virtual == NULL ||
        vmm_pml4_physical == 0) {

        return false;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return false;
    }

    uint64_t kernel_entry =
        __atomic_load_n(
            &vmm_pml4_virtual[pml4_index],
            __ATOMIC_ACQUIRE
        );

    /*
     * Non-present entries must be completely zero.
     */
    if (!(kernel_entry & VMM_PRESENT)) {
        if (kernel_entry != 0) {
            return false;
        }
    } else {

        /*
         * PS is reserved in a PML4 entry.
         */
        if (kernel_entry & VMM_HUGE_PAGE) {
            return false;
        }

        /*
         * Reject physical address bits unsupported by
         * the detected CPU physical-address width.
         */
        uint64_t physical_field_mask =
            ((1ULL << 52) - 1ULL) & ~VMM_PAGE_MASK;

        if (kernel_entry &
            physical_field_mask &
            ~address_mask) {

            return false;
        }

        uint64_t child_physical =
            kernel_entry & address_mask;

        if (child_physical == 0 ||
            (child_physical & VMM_PAGE_MASK) != 0 ||
            vmm_get_table(child_physical) == NULL) {

            return false;
        }
    }

    /*
     * Process roots always have USER cleared at the
     * top-level kernel entry.
     */
    uint64_t expected_entry =
        kernel_entry & ~VMM_USER;

    /*
     * Validate every registered process address space.
     * Do not modify anything during this pass.
     */
    for (
        vmm_address_space_t *space = vmm_address_space_list;
        space != NULL;
        space = space->next
    ) {

        if (!space->registered ||
            space->is_kernel ||
            space->reference_count == 0 ||
            space->root_physical == 0 ||
            (space->root_physical & VMM_PAGE_MASK) != 0 ||
            space->root_virtual == NULL ||
            ((uintptr_t)space->root_virtual & VMM_PAGE_MASK) != 0 ||
            pmm_frame_to_virt(space->root_physical) !=
                (void *)space->root_virtual) {

            return false;
        }

        uint64_t process_entry =
            __atomic_load_n(
                &space->root_virtual[pml4_index],
                __ATOMIC_ACQUIRE
            );

        if (process_entry != expected_entry) {
            return false;
        }
    }

    return true;
}


/*
 * Synchronize one kernel upper-half PML4 entry into every
 * registered process address space.
 *
 * The caller must:
 *   1. Hold vmm_lock.
 *   2. Have successfully called
 *      vmm_validate_kernel_pml4_mirror_locked() before
 *      modifying the kernel root.
 *
 * The function cannot fail after successful preflight.
 */
static void vmm_sync_kernel_pml4_mirror_locked(
    uint64_t pml4_index
) {
    uint64_t kernel_entry =
        __atomic_load_n(
            &vmm_pml4_virtual[pml4_index],
            __ATOMIC_ACQUIRE
        );

    /*
     * Kernel mappings must remain supervisor-only in
     * independently created address spaces.
     */
    uint64_t mirrored_entry =
        kernel_entry & ~VMM_USER;

    for (
        vmm_address_space_t *space = vmm_address_space_list;
        space != NULL;
        space = space->next
    ) {

        __atomic_store_n(
            &space->root_virtual[pml4_index],
            mirrored_entry,
            __ATOMIC_RELEASE
        );
    }
}

/*
 * TEST HOOK:
 * Inject a duplicate reference to an existing private PML4
 * child table through a currently empty lower-half PML4 slot.
 *
 * This deliberately creates an invalid ownership topology:
 * one owned page table becomes reachable through two root
 * entries.
 *
 * The VMM lock must protect the complete modification.
 */
bool vmm_test_inject_duplicate_root_entry(
    vmm_address_space_t *space,
    uint64_t *alias_index_out,
    uint64_t *duplicated_entry_out
) {
    if (!vmm_initialized ||
        space == NULL ||
        space == &vmm_kernel_space_object ||
        alias_index_out == NULL ||
        duplicated_entry_out == NULL) {

        return false;
    }

    *alias_index_out = 0;
    *duplicated_entry_out = 0;

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    bool result = false;

    if (!vmm_address_space_is_registered_unlocked(space) ||
        !space->registered ||
        space->is_kernel ||
        space->root_physical == 0 ||
        (space->root_physical & VMM_PAGE_MASK) != 0 ||
        space->root_virtual == NULL ||
        pmm_frame_to_virt(space->root_physical) !=
            (void *)space->root_virtual) {

        goto duplicate_injection_done;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        goto duplicate_injection_done;
    }

    uint64_t source_entry = 0;
    uint64_t alias_index = UINT64_MAX;

    /*
     * Find:
     *   1. One valid existing private root entry.
     *   2. One completely empty private root entry.
     *
     * Non-present entries containing residual bits are
     * rejected instead of being treated as empty.
     */
    for (uint64_t i = 0; i < 256; i++) {

        uint64_t entry =
            __atomic_load_n(
                &space->root_virtual[i],
                __ATOMIC_ACQUIRE
            );

        if (entry == 0) {

            if (alias_index == UINT64_MAX) {
                alias_index = i;
            }

            continue;
        }

        if (!(entry & VMM_PRESENT) ||
            (entry & VMM_HUGE_PAGE)) {

            goto duplicate_injection_done;
        }

        uint64_t child_physical =
            entry & address_mask;

        if (child_physical == 0 ||
            (child_physical & VMM_PAGE_MASK) != 0 ||
            pmm_frame_to_virt(child_physical) == NULL) {

            goto duplicate_injection_done;
        }

        uint64_t recorded_owner = 0;

        if (!vmm_find_owned_table(
                child_physical,
                &recorded_owner
            ) ||
            recorded_owner != space->root_physical) {

            goto duplicate_injection_done;
        }

        if (source_entry == 0) {
            source_entry = entry;
        }
    }

    if (source_entry == 0 ||
        alias_index == UINT64_MAX) {

        goto duplicate_injection_done;
    }

    /*
     * Publish the duplicate reference only if the target
     * slot is still exactly zero.
     */
    uint64_t expected_empty = 0;

    if (!__atomic_compare_exchange_n(
            &space->root_virtual[alias_index],
            &expected_empty,
            source_entry,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE
        )) {

        goto duplicate_injection_done;
    }

    *alias_index_out = alias_index;
    *duplicated_entry_out = source_entry;

    result = true;

duplicate_injection_done:

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}


/*
 * TEST HOOK:
 * Restore the root slot modified by
 * vmm_test_inject_duplicate_root_entry().
 *
 * The function refuses to clear the slot unless:
 *   - The address space is still registered.
 *   - The selected slot contains the exact injected entry.
 *   - Another root slot still references that same entry.
 *   - The referenced table belongs to this address space.
 */
bool vmm_test_restore_duplicate_root_entry(
    vmm_address_space_t *space,
    uint64_t alias_index,
    uint64_t duplicated_entry
) {
    if (!vmm_initialized ||
        space == NULL ||
        space == &vmm_kernel_space_object ||
        alias_index >= 256 ||
        duplicated_entry == 0 ||
        !(duplicated_entry & VMM_PRESENT) ||
        (duplicated_entry & VMM_HUGE_PAGE)) {

        return false;
    }

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    bool result = false;

    if (!vmm_address_space_is_registered_unlocked(space) ||
        !space->registered ||
        space->is_kernel ||
        space->root_physical == 0 ||
        space->root_virtual == NULL) {

        goto duplicate_restore_done;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        goto duplicate_restore_done;
    }

    uint64_t child_physical =
        duplicated_entry & address_mask;

    uint64_t recorded_owner = 0;

    if (child_physical == 0 ||
        (child_physical & VMM_PAGE_MASK) != 0 ||
        !vmm_find_owned_table(
            child_physical,
            &recorded_owner
        ) ||
        recorded_owner != space->root_physical) {

        goto duplicate_restore_done;
    }

    /*
     * Do not restore if the alias slot no longer contains
     * the exact value installed by the injection hook.
     */
    uint64_t current_entry =
        __atomic_load_n(
            &space->root_virtual[alias_index],
            __ATOMIC_ACQUIRE
        );

    if (current_entry != duplicated_entry) {
        goto duplicate_restore_done;
    }

    /*
     * Ensure another root entry still contains the original
     * reference. Never clear the only remaining reference.
     */
    bool original_reference_found = false;

    for (uint64_t i = 0; i < 256; i++) {

        if (i == alias_index) {
            continue;
        }

        uint64_t entry =
            __atomic_load_n(
                &space->root_virtual[i],
                __ATOMIC_ACQUIRE
            );

        if (entry == duplicated_entry) {
            original_reference_found = true;
            break;
        }
    }

    if (!original_reference_found) {
        goto duplicate_restore_done;
    }

    /*
     * Remove only the exact injected entry.
     */
    uint64_t expected_entry = duplicated_entry;

    if (!__atomic_compare_exchange_n(
            &space->root_virtual[alias_index],
            &expected_entry,
            0,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE
        )) {

        goto duplicate_restore_done;
    }

    result = true;

duplicate_restore_done:

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}

/*
 * TEST HOOK:
 * Allocate a page-table frame and register it as belonging
 * to an address space, without connecting it to the page-table
 * hierarchy.
 *
 * This deliberately creates an orphan ownership record.
 *
 * The frame is allocated before acquiring vmm_lock.
 * Registration and test-state publication happen while locked.
 */
bool vmm_test_inject_orphan_owned_table(
    vmm_address_space_t *space,
    uint64_t *orphan_frame_out
) {
    if (!vmm_initialized ||
        space == NULL ||
        space == &vmm_kernel_space_object ||
        orphan_frame_out == NULL) {

        return false;
    }

    *orphan_frame_out = 0;

    uint64_t orphan_frame = 0;

    volatile uint64_t *orphan_virtual =
        vmm_allocate_table_frame(&orphan_frame);

    if (orphan_virtual == NULL) {
        return false;
    }

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        pmm_free_frame(orphan_frame);
        return false;
    }

    bool result = false;

    if (!vmm_address_space_is_registered_unlocked(space) ||
        !space->registered ||
        space->is_kernel ||
        space->reference_count == 0 ||
        space->root_physical == 0 ||
        (space->root_physical & VMM_PAGE_MASK) != 0 ||
        space->root_virtual == NULL ||
        pmm_frame_to_virt(space->root_physical) !=
            (void *)space->root_virtual) {

        goto orphan_injection_done;
    }

    /*
     * Only one orphan injection may be outstanding.
     */
    if (vmm_test_orphan_frame != 0 ||
        vmm_test_orphan_owner_root != 0) {

        goto orphan_injection_done;
    }

    /*
     * A newly allocated frame must not already have an
     * ownership record.
     */
    if (vmm_find_owned_table(orphan_frame, NULL)) {
        goto orphan_injection_done;
    }

    /*
     * Register the frame but deliberately do not publish
     * it in any page-table parent entry.
     */
    if (!vmm_register_owned_table(
            orphan_frame,
            space->root_physical
        )) {

        goto orphan_injection_done;
    }

    vmm_test_orphan_frame = orphan_frame;
    vmm_test_orphan_owner_root = space->root_physical;

    *orphan_frame_out = orphan_frame;

    result = true;

orphan_injection_done:

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    /*
     * If registration failed, the allocated frame remains
     * unowned and must be returned to PMM.
     */
    if (!result) {
        pmm_free_frame(orphan_frame);
    }

    return result;
}


/*
 * TEST HOOK:
 * Remove the exact orphan record installed by
 * vmm_test_inject_orphan_owned_table().
 *
 * The physical frame is freed only after its ownership
 * record has been successfully removed.
 */
bool vmm_test_restore_orphan_owned_table(
    vmm_address_space_t *space,
    uint64_t orphan_frame
) {
    if (!vmm_initialized ||
        space == NULL ||
        space == &vmm_kernel_space_object ||
        orphan_frame == 0 ||
        (orphan_frame & VMM_PAGE_MASK) != 0) {

        return false;
    }

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    bool result = false;

    if (!vmm_address_space_is_registered_unlocked(space) ||
        !space->registered ||
        space->is_kernel ||
        space->root_physical == 0 ||
        space->root_virtual == NULL) {

        goto orphan_restore_done;
    }

    /*
     * Match the exact outstanding test injection.
     */
    if (vmm_test_orphan_frame != orphan_frame ||
        vmm_test_orphan_owner_root != space->root_physical) {

        goto orphan_restore_done;
    }

    /*
     * Confirm the registry still contains the exact
     * expected owner record.
     */
    uint64_t recorded_owner = 0;

    if (!vmm_find_owned_table(
            orphan_frame,
            &recorded_owner
        ) ||
        recorded_owner != space->root_physical ||
        pmm_frame_to_virt(orphan_frame) == NULL) {

        goto orphan_restore_done;
    }

    if (!vmm_unregister_owned_table(
            orphan_frame,
            space->root_physical
        )) {

        goto orphan_restore_done;
    }

    /*
     * The registry record is now removed.
     * Release its physical frame while still holding
     * vmm_lock, consistent with VMM teardown's locking.
     */
    if (!pmm_free_frame(orphan_frame)) {

        /*
         * Preserve the orphan registration if PMM refuses
         * to release the frame. The registry node should
         * have the capacity just freed by unregistering.
         */
        vmm_register_owned_table(
            orphan_frame,
            space->root_physical
        );

        goto orphan_restore_done;
    }

    vmm_test_orphan_frame = 0;
    vmm_test_orphan_owner_root = 0;

    result = true;

orphan_restore_done:

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}

/*
 * Return the permanent kernel address-space object.
 */
vmm_address_space_t *vmm_kernel_address_space(void) {

    if (!vmm_initialized) {
        return NULL;
    }

    return &vmm_kernel_space_object;
}


/*
 * Create an independent address space.
 *
 * The PML4 root is private.
 * Existing upper-half kernel mappings are shared.
 * Lower-half entries remain empty.
 *
 * The function deliberately allocates its descriptor before
 * acquiring the address-space lock. Heap allocation may itself
 * use VMM, so allocating under the VMM lock would risk recursion.
 */
vmm_address_space_t *vmm_address_space_create(void) {

    if (!vmm_initialized) {
        return NULL;
    }

    /*
     * Requires heap_init() to have completed.
     */
    vmm_address_space_t *space =
        kmalloc(sizeof(vmm_address_space_t));

    if (space == NULL) {
        return NULL;
    }

    space->root_physical = 0;
    space->root_virtual = NULL;
    space->reference_count = 0;
    space->active_cpu_mask = 0;
    space->is_kernel = false;
    space->registered = false;
    space->next = NULL;


    /*
     * Serialize root creation with other address-space
     * lifecycle operations.
     */
    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        kfree(space);
        return NULL;
    }


    /*
     * Allocate one private PML4 frame.
     *
     * PMM zeroes newly allocated frames.
     */
    uint64_t root_physical = pmm_alloc_frame();

    if (root_physical == 0) {

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        kfree(space);
        return NULL;
    }


    volatile uint64_t *root =
        (volatile uint64_t *)pmm_frame_to_virt(
            root_physical
        );

    if (root == NULL ||
        ((uintptr_t)root & VMM_PAGE_MASK) != 0) {

        pmm_free_frame(root_physical);

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        kfree(space);
        return NULL;
    }


    /*
     * Start from an entirely empty PML4.
     *
     * This explicitly guarantees that the user half cannot
     * inherit any bootloader or kernel mapping.
     */
    for (uint64_t i = 0; i < 512; i++) {

        __atomic_store_n(
            &root[i],
            0,
            __ATOMIC_RELAXED
        );
    }


    /*
     * Copy the upper-half kernel PML4 entries.
     *
     * Four-level x86-64 paging divides the PML4 into:
     *
     *   0..255   lower canonical half
     *   256..511 upper canonical half
     *
     * Each copied entry references the same lower-level
     * kernel page tables as the permanent kernel root.
     *
     * Clear U/S at the copied PML4 level. This ensures
     * inherited kernel mappings remain supervisor-only,
     * even if lower-level table entries are permissive.
     *
     * The original kernel root is never modified here.
     */
    for (uint64_t i = 256; i < 512; i++) {

        uint64_t entry =
            __atomic_load_n(
                &vmm_pml4_virtual[i],
                __ATOMIC_ACQUIRE
            );

        /*
         * PS is reserved in a PML4 entry.
         */
        if (entry & VMM_HUGE_PAGE) {

            pmm_free_frame(root_physical);

            raw_spinlock_unlock_irqrestore(
                &vmm_lock,
                irq_flags
            );

            kfree(space);
            return NULL;
        }

        if (entry & VMM_PRESENT) {

            entry &= ~VMM_USER;

            __atomic_store_n(
                &root[i],
                entry,
                __ATOMIC_RELAXED
            );
        }
    }


    /*
     * Initialize object ownership.
     */
    space->root_physical = root_physical;
    space->root_virtual = root;
    space->reference_count = 1;
    space->is_kernel = false;
    space->registered = true;


    /*
     * Insert into the address-space registry.
     */
    space->next = vmm_address_space_list;
    vmm_address_space_list = space;


    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return space;
}


/*
 * Acquire another reference to an address space.
 */
bool vmm_address_space_get(vmm_address_space_t *space) {

    if (!vmm_initialized || space == NULL) {
        return false;
    }

    if (space == &vmm_kernel_space_object) {
        return true;
    }

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {
        return false;
    }

    bool result = false;

    if (vmm_address_space_is_registered_unlocked(space) &&
        space->reference_count > 0 &&
        space->reference_count < UINT64_MAX) {

        space->reference_count++;

        result = true;
    }

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}


/*
 * Validate one private page-table subtree before destruction.
 *
 * level:
 *     3 = PDPT
 *     2 = PD
 *     1 = PT
 *
 * Every intermediate table must:
 *     - Be allocated by PMM.
 *     - Be accessible through the HHDM.
 *     - Have an ownership record for this exact address space.
 *
 * Huge pages are rejected because process mappings currently
 * use 4 KiB pages exclusively.
 *
 * The VMM lock must be held by the caller.
 */
static bool vmm_validate_owned_table_tree(
    uint64_t table_physical,
    uint64_t owner_root_physical,
    uint32_t level
) {
    if (table_physical == 0 ||
        owner_root_physical == 0 ||
        (table_physical & VMM_PAGE_MASK) != 0 ||
        (owner_root_physical & VMM_PAGE_MASK) != 0 ||
        level < 1 ||
        level > 3) {

        return false;
    }

    if (
        !vmm_mark_owned_table_visited(
            table_physical,
            owner_root_physical
        )
    ) {
        return false;
    }

    /*
     * vmm_get_table() verifies HHDM accessibility.
     * pmm_frame_to_virt() additionally verifies that PMM
     * currently considers this frame allocated.
     */
    volatile uint64_t *table =
        vmm_get_table(table_physical);

    if (table == NULL ||
        pmm_frame_to_virt(table_physical) == NULL) {

        return false;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return false;
    }

    for (uint64_t i = 0; i < 512; i++) {

        uint64_t entry =
            __atomic_load_n(
                &table[i],
                __ATOMIC_ACQUIRE
            );

        if (!(entry & VMM_PRESENT)) {

            /*
             * Our current page-table implementation clears
             * unused entries completely. Nonzero nonpresent
             * entries are unexpected and must not be silently
             * interpreted as valid hierarchy.
             */
            if (entry != 0) {
                return false;
            }

            continue;
        }

        /*
         * PT entries are leaf mappings. Bit 7 is PAT there,
         * not the huge-page flag.
         */
        if (level == 1) {
            continue;
        }

        /*
         * Process address spaces currently support only
         * 4 KiB mappings.
         */
        if (entry & VMM_HUGE_PAGE) {
            return false;
        }

        uint64_t child_physical =
            entry & address_mask;

        if (child_physical == 0 ||
            (child_physical & VMM_PAGE_MASK) != 0) {

            return false;
        }

        if (!vmm_validate_owned_table_tree(
                child_physical,
                owner_root_physical,
                level - 1
            )) {

            return false;
        }
    }

    return true;
}


/*
 * Recursively detach and reclaim one validated private
 * page-table subtree.
 *
 * level:
 *     3 = PDPT
 *     2 = PD
 *     1 = PT
 *
 * The caller must have completed the entire-tree validation
 * pass before invoking this function.
 *
 * The VMM lock must be held by the caller.
 *
 * Leaf physical data frames are never released here.
 */
static bool vmm_reclaim_owned_table_tree(
    volatile uint64_t *parent_entry,
    uint64_t owner_root_physical,
    uint32_t level
) {
    if (parent_entry == NULL ||
        owner_root_physical == 0 ||
        level < 1 ||
        level > 3) {

        return false;
    }

    uint64_t entry =
        __atomic_load_n(
            parent_entry,
            __ATOMIC_ACQUIRE
        );

    if (!(entry & VMM_PRESENT) ||
        (entry & VMM_HUGE_PAGE)) {

        return false;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return false;
    }

    uint64_t table_physical =
        entry & address_mask;

    if (table_physical == 0 ||
        (table_physical & VMM_PAGE_MASK) != 0) {

        return false;
    }

    uint64_t recorded_owner = 0;

    if (!vmm_find_owned_table(
            table_physical,
            &recorded_owner
        ) ||
        recorded_owner != owner_root_physical) {

        return false;
    }

    volatile uint64_t *table =
        vmm_get_table(table_physical);

    if (table == NULL) {
        return false;
    }

    /*
     * Reclaim descendants first.
     *
     * A level-1 table contains only leaf mappings, whose
     * physical frames belong to the mapping caller.
     */
    if (level > 1) {

        for (uint64_t i = 0; i < 512; i++) {

            uint64_t child_entry =
                __atomic_load_n(
                    &table[i],
                    __ATOMIC_ACQUIRE
                );

            if (!(child_entry & VMM_PRESENT)) {
                continue;
            }

            if (!vmm_reclaim_owned_table_tree(
                    &table[i],
                    owner_root_physical,
                    level - 1
                )) {

                return false;
            }
        }
    }

    /*
     * Detach this table from its parent.
     *
     * The complete hierarchy was prevalidated, and the
     * VMM lock prevents concurrent VMM modifications.
     */
    uint64_t expected_entry = entry;

    if (!__atomic_compare_exchange_n(
            parent_entry,
            &expected_entry,
            0,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {

        return false;
    }

    /*
    * Release the detached table transactionally.
    *
    * Address-space destruction requires the address space to
    * be inactive, so no local TLB invalidation is necessary.
    */
    return vmm_release_detached_owned_table_locked(
        parent_entry,
        entry,
        owner_root_physical,
        table_physical
    );
}



/*
 * Release one address-space reference.
 *
 * On the final reference:
 *     1. Validate the complete private lower-half hierarchy.
 *     2. Reclaim all private intermediate page tables.
 *     3. Release the private PML4 root.
 *     4. Unregister and destroy the address-space object.
 *
 * Shared upper-half kernel page tables are never reclaimed
 * through this function.
 *
 * Mapped data frames remain owned by their callers.
 */
bool vmm_address_space_put(vmm_address_space_t *space) {

    if (!vmm_initialized ||
        space == NULL ||
        space == &vmm_kernel_space_object) {

        return false;
    }

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    /*
     * Locate the exact registered object before dereferencing
     * the supplied pointer.
     */
    vmm_address_space_t **link =
        &vmm_address_space_list;

    while (*link != NULL && *link != space) {
        link = &(*link)->next;
    }

    if (*link != space ||
        !space->registered ||
        space->reference_count == 0) {

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        return false;
    }

    /*
     * More than one reference exists.
     * Release only this reference.
     */
    if (space->reference_count > 1) {

        space->reference_count--;

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        return true;
    }

    /*
    * A final reference cannot be destroyed while the
    * address space is active on any logical CPU.
    */
    if (space->active_cpu_mask != 0) {

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        return false;
    }

    /*
    * Cross-check the per-CPU pointers as well.
    *
    * The active mask and active-space array must agree.
    */
    for (uint32_t cpu = 0; cpu < VMM_MAX_CPUS; cpu++) {

        if (vmm_active_address_spaces[cpu] == space) {

            raw_spinlock_unlock_irqrestore(
                &vmm_lock,
                irq_flags
            );

            return false;
        }
    }

    /*
     * Final reference.
     *
     * No CR3 switching is implemented yet. Therefore, this
     * independently created address space cannot be active.
     */

    if (space->root_physical == 0 ||
        (space->root_physical & VMM_PAGE_MASK) != 0 ||
        space->root_virtual == NULL ||
        pmm_frame_to_virt(space->root_physical) !=
            (void *)space->root_virtual) {

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        return false;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        return false;
    }

    /*
    * Start a fresh validation pass.
    *
    * Every record belonging to this address space must begin
    * with validation_visited == false.
    */
    for (
        vmm_owned_table_node_t *node = vmm_owned_tables;
        node != NULL;
        node = node->next
    ) {
        for (uint64_t i = 0; i < node->count; i++) {
            vmm_owned_table_record_t *record =
                &node->records[i];

            if (
                record->owner_root_physical ==
                space->root_physical
            ) {
                record->validation_visited = false;
            }
        }
    }

    /*
     * PASS 1: Validate the complete private lower-half tree.
     *
     * PML4 entries 0..255 are process-private.
     * Entries 256..511 are shared kernel mappings and must
     * never be traversed or reclaimed here.
     */
    for (uint64_t i = 0; i < 256; i++) {

        uint64_t entry =
            __atomic_load_n(
                &space->root_virtual[i],
                __ATOMIC_ACQUIRE
            );

        if (!(entry & VMM_PRESENT)) {

            if (entry != 0) {

                raw_spinlock_unlock_irqrestore(
                    &vmm_lock,
                    irq_flags
                );

                return false;
            }

            continue;
        }

        /*
         * Huge pages are not valid at the PML4 level.
         */
        if (entry & VMM_HUGE_PAGE) {

            raw_spinlock_unlock_irqrestore(
                &vmm_lock,
                irq_flags
            );

            return false;
        }

        uint64_t child_physical =
            entry & address_mask;

        if (child_physical == 0 ||
            (child_physical & VMM_PAGE_MASK) != 0 ||
            !vmm_validate_owned_table_tree(
                child_physical,
                space->root_physical,
                3
            )) {

            raw_spinlock_unlock_irqrestore(
                &vmm_lock,
                irq_flags
            );

            return false;
        }
    }

    /*
    * Every registered table owned by this address space must
    * have been reached exactly once from its root.
    *
    * An unvisited record is an orphan: it is registered as
    * belonging to this address space but is not reachable
    * through the page-table hierarchy.
    */
    for (
        vmm_owned_table_node_t *node = vmm_owned_tables;
        node != NULL;
        node = node->next
    ) {
        for (uint64_t i = 0; i < node->count; i++) {
            vmm_owned_table_record_t *record =
                &node->records[i];

            if (
                record->owner_root_physical ==
                    space->root_physical &&
                !record->validation_visited
            ) {
                raw_spinlock_unlock_irqrestore(
                    &vmm_lock,
                    irq_flags
                );

                return false;
            }
        }
    }

    /*
     * PASS 2: Reclaim all validated private page-table trees.
     *
     * Descendants are reclaimed before their parent tables.
     */
    for (uint64_t i = 0; i < 256; i++) {

        uint64_t entry =
            __atomic_load_n(
                &space->root_virtual[i],
                __ATOMIC_ACQUIRE
            );

        if (!(entry & VMM_PRESENT)) {
            continue;
        }

        if (!vmm_reclaim_owned_table_tree(
                &space->root_virtual[i],
                space->root_physical,
                3
            )) {

            /*
             * A failure here indicates an unexpected invariant
             * violation or PMM failure. Do not release the root
             * or descriptor. Any already detached tables remain
             * detached and are never reused unsafely.
             */
            raw_spinlock_unlock_irqrestore(
                &vmm_lock,
                irq_flags
            );

            return false;
        }
    }

    /*
     * All private lower-half page tables have now been detached
     * and released. Shared upper-half entries remain untouched.
     *
     * Release the private PML4 root.
     */
    if (!pmm_free_frame(space->root_physical)) {

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        return false;
    }

    /*
     * Remove the address-space object from the registry.
     */
    *link = space->next;

    space->registered = false;
    space->reference_count = 0;
    space->root_physical = 0;
    space->root_virtual = NULL;
    space->next = NULL;

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    /*
     * Heap operations must remain outside vmm_lock.
     */
    kfree(space);

    return true;
}


/*
 * Query an address space without loading its root into CR3.
 */
vmm_status_t vmm_address_space_query(
    vmm_address_space_t *space,
    uint64_t virtual_address,
    vmm_mapping_info_t *mapping
) {

    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (space == NULL || mapping == NULL) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }


    if (!vmm_address_space_is_registered_unlocked(space)) {

        raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            irq_flags
        );

        return VMM_STATUS_INVALID_ARGUMENT;
    }


    vmm_status_t status =
        vmm_query_root(
            space->root_virtual,
            virtual_address,
            mapping
        );


    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return status;
}


/*
 * Retrieve the physical PML4 root address.
 */
bool vmm_address_space_get_root_physical(
    const vmm_address_space_t *space,
    uint64_t *physical_address_out
) {

    if (!vmm_initialized ||
        space == NULL ||
        physical_address_out == NULL) {

        return false;
    }

    *physical_address_out = 0;

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {
        return false;
    }


    bool result = false;

    if (vmm_address_space_is_registered_unlocked(space)) {

        *physical_address_out =
            space->root_physical;

        result = true;
    }


    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}

/*
 * Activate an address space on CPU0.
 *
 * The VMM lock protects:
 * - Address-space lifetime.
 * - Active CPU masks.
 * - Per-CPU active address-space records.
 * - CR3 transition bookkeeping.
 *
 * Interrupts remain disabled while the lock is held.
 *
 * Only CPU0 is currently supported. SMP activation requires
 * CPU-local execution verification and TLB shootdown support.
 */
bool vmm_address_space_activate(
    uint32_t logical_cpu,
    vmm_address_space_t *space
) {
    if (!vmm_initialized ||
        space == NULL ||
        logical_cpu >= VMM_MAX_CPUS ||
        logical_cpu != 0) {

        return false;
    }

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    bool result = false;

    uint64_t cpu_bit =
        1ULL << logical_cpu;

    /*
     * Validate the supplied pointer against the registry
     * before dereferencing it.
     */
    if (!vmm_address_space_is_registered_unlocked(space)) {
        goto activation_done;
    }

    /*
     * Validate the target root.
     */
    if (space->root_physical == 0 ||
        (space->root_physical & VMM_PAGE_MASK) != 0 ||
        space->root_virtual == NULL) {

        goto activation_done;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0 ||
        (space->root_physical & ~address_mask) != 0) {

        goto activation_done;
    }

    if (space->is_kernel) {

        /*
         * The kernel address-space object must always
         * refer to the original Limine root.
         */
        if (space != &vmm_kernel_space_object ||
            space->root_physical != vmm_pml4_physical ||
            space->root_virtual != vmm_pml4_virtual) {

            goto activation_done;
        }

    } else {

        /*
         * A process root must remain backed by its
         * registered PMM frame.
         */
        if (!space->registered ||
            space->reference_count == 0 ||
            pmm_frame_to_virt(space->root_physical) !=
                (void *)space->root_virtual) {

            goto activation_done;
        }
    }

    /*
     * Locate the address space currently active on CPU0.
     */
    vmm_address_space_t *old_space =
        vmm_active_address_spaces[logical_cpu];

    if (old_space == NULL ||
        !vmm_address_space_is_registered_unlocked(
            old_space
        )) {

        goto activation_done;
    }

    /*
     * The outgoing address space must have an active
     * CPU0 bit.
     */
    if (!(old_space->active_cpu_mask & cpu_bit)) {
        goto activation_done;
    }

    /*
     * A private address space cannot already be active
     * on a different CPU in this CPU0-only implementation.
     */
    if (space->active_cpu_mask & ~cpu_bit) {
        goto activation_done;
    }

    /*
     * Read the hardware CR3 and verify that software
     * tracking still describes the actual CPU state.
     */
    uint64_t current_cr3 = 0;

    asm volatile (
        "mov %%cr3, %0"
        : "=r"(current_cr3)
        :
        : "memory"
    );

    uint64_t current_root =
        current_cr3 & address_mask;

    if (current_root != old_space->root_physical) {
        goto activation_done;
    }

    /*
     * Switching to the already active address space
     * is valid, but only if the software and hardware
     * states agree.
     */
    if (old_space == space) {
        result = true;
        goto activation_done;
    }

    /*
     * The target must not already claim CPU0 while
     * another address space is recorded as active.
     */
    if (space->active_cpu_mask & cpu_bit) {
        goto activation_done;
    }

    /*
     * Validate every shared upper-half kernel PML4 entry.
     *
     * This prevents activation of a process root containing
     * a stale or incorrectly permissioned kernel mapping.
     */
    for (uint64_t i = 256; i < 512; i++) {

        if (!vmm_validate_kernel_pml4_mirror_locked(i)) {
            goto activation_done;
        }
    }

    /*
     * Prepare the target CR3.
     *
     * Bit 63 is deliberately clear. When PCID is enabled,
     * this requests invalidation for the selected PCID.
     *
     * We currently use the same retained low control bits
     * for every address space, avoiding stale translations
     * being reused between different roots.
     */
    uint64_t new_cr3 =
        space->root_physical |
        vmm_cr3_low_flags;

    /*
     * Load the new root.
     *
     * Kernel code, data and stack remain accessible because
     * the upper-half kernel mappings are shared.
     */
    asm volatile (
        "mov %0, %%cr3"
        :
        : "r"(new_cr3)
        : "memory"
    );

    /*
     * Read back the processor's actual CR3.
     */
    uint64_t verified_cr3 = 0;

    asm volatile (
        "mov %%cr3, %0"
        : "=r"(verified_cr3)
        :
        : "memory"
    );

    if ((verified_cr3 & address_mask) !=
        space->root_physical) {

        /*
         * The target root was not installed as expected.
         * Attempt to restore the previous root.
         */
        uint64_t old_cr3 =
            old_space->root_physical |
            vmm_cr3_low_flags;

        asm volatile (
            "mov %0, %%cr3"
            :
            : "r"(old_cr3)
            : "memory"
        );

        goto activation_done;
    }

    /*
     * Hardware activation succeeded.
     * Now update software ownership bookkeeping.
     */
    old_space->active_cpu_mask &= ~cpu_bit;

    space->active_cpu_mask |= cpu_bit;

    vmm_active_address_spaces[logical_cpu] = space;

    result = true;

activation_done:

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}

/*
 * TEST HOOK:
 * Read the currently active address space and verify
 * that the recorded root matches hardware CR3.
 */
bool vmm_test_get_active_address_space(
    uint32_t logical_cpu,
    vmm_address_space_t **space_out,
    uint64_t *root_physical_out
) {
    if (!vmm_initialized ||
        logical_cpu >= VMM_MAX_CPUS ||
        space_out == NULL ||
        root_physical_out == NULL) {

        return false;
    }

    *space_out = NULL;
    *root_physical_out = 0;

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    bool result = false;

    vmm_address_space_t *space =
        vmm_active_address_spaces[logical_cpu];

    if (space == NULL ||
        !vmm_address_space_is_registered_unlocked(space)) {

        goto active_test_done;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    uint64_t current_cr3 = 0;

    asm volatile (
        "mov %%cr3, %0"
        : "=r"(current_cr3)
        :
        : "memory"
    );

    uint64_t root_physical =
        current_cr3 & address_mask;

    if (root_physical != space->root_physical ||
        !(space->active_cpu_mask & (1ULL << logical_cpu))) {

        goto active_test_done;
    }

    *space_out = space;
    *root_physical_out = root_physical;

    result = true;

active_test_done:

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}


/*
 * TEST HOOK:
 * Read an address space's active CPU mask.
 */
bool vmm_test_get_address_space_active_cpu_mask(
    const vmm_address_space_t *space,
    uint64_t *mask_out
) {
    if (!vmm_initialized ||
        space == NULL ||
        mask_out == NULL) {

        return false;
    }

    *mask_out = 0;

    uint64_t irq_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &irq_flags
        )) {

        return false;
    }

    bool result = false;

    if (vmm_address_space_is_registered_unlocked(space)) {

        *mask_out = space->active_cpu_mask;
        result = true;
    }

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        irq_flags
    );

    return result;
}

/*
 * Remove a newly created intermediate page table during
 * rollback, but only if:
 *
 * 1. The table is registered as VMM-owned.
 * 2. The table belongs to the expected address space.
 * 3. The table is completely empty.
 * 4. The parent entry still points to this table.
 *
 * Existing Limine page tables are never reclaimed.
 *
 * Caller must hold vmm_lock.
 */
static bool vmm_rollback_created_table(
    volatile uint64_t *parent_entry,
    uint64_t virtual_address,
    uint64_t owner_root_physical
) {
    if (parent_entry == 0 ||
        owner_root_physical == 0 ||
        (owner_root_physical & VMM_PAGE_MASK) != 0) {

        return false;
    }

    uint64_t entry =
        __atomic_load_n(parent_entry, __ATOMIC_ACQUIRE);

    if (!(entry & VMM_PRESENT) ||
        (entry & VMM_HUGE_PAGE)) {

        return false;
    }

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return false;
    }

    uint64_t table_physical =
        entry & address_mask;

    /*
     * Verify that this table belongs to the address space
     * whose hierarchy is being modified.
     */
    uint64_t recorded_owner = 0;

    if (!vmm_find_owned_table(
            table_physical,
            &recorded_owner
        )) {

        return false;
    }

    if (recorded_owner != owner_root_physical) {
        return false;
    }

    volatile uint64_t *table =
        vmm_get_table(table_physical);

    if (table == 0) {
        return false;
    }

    /*
     * A table must be empty before it can be reclaimed.
     */
    for (uint64_t i = 0; i < 512; i++) {

        if (__atomic_load_n(
                &table[i],
                __ATOMIC_ACQUIRE) != 0) {

            return false;
        }
    }

    /*
     * Detach the table only if the parent entry has not changed.
     */
    uint64_t expected = entry;

    if (!__atomic_compare_exchange_n(
            parent_entry,
            &expected,
            0,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {

        return false;
    }

    /*
     * Flush the translation and paging-structure information
     * associated with this virtual address.
     */
    vmm_invalidate_page(virtual_address);

   /*
    * Release the detached table transactionally.
    *
    * If PMM refuses the free, the helper restores the original
    * parent entry and ownership record.
    */
    bool released =
        vmm_release_detached_owned_table_locked(
            parent_entry,
            entry,
            owner_root_physical,
            table_physical
        );

    if (!released) {

        /*
        * The helper may have restored the original parent entry.
        * Invalidate again so the CPU observes the restored tree.
        */
        vmm_invalidate_page(virtual_address);
    }

    return released;
}

/*
 * Forward declaration.
 *
 * vmm_map_range() uses this helper for rollback before
 * its implementation appears later in this file.
 */
static vmm_status_t vmm_unmap_page_root_locked(
    volatile uint64_t *root_virtual,
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t *physical_address
);

static vmm_status_t vmm_unmap_page_locked(
    uint64_t virtual_address,
    uint64_t *physical_address
);

/*
 * Map one 4 KiB physical frame into the active address space.
 *
 * The physical data frame remains owned by the caller.
 * Only newly created intermediate page tables belong to VMM.
 */
static vmm_status_t vmm_map_page_root_locked(
    volatile uint64_t *root_virtual,
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags,
    bool process_mapping
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (root_virtual == NULL ||
        root_physical == 0 ||
        (root_physical & VMM_PAGE_MASK) != 0) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate the virtual address.
     */
    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * Process address spaces may only map the lower
     * canonical half of the x86_64 virtual address space.
     */
    if (process_mapping &&
        virtual_address >= 0x0000800000000000ULL) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
    * Process address spaces may contain both user-accessible
    * and supervisor-only mappings.
    *
    * User accessibility is enforced by the effective U/S
    * permissions across the complete page-table hierarchy.
    *
    * A supervisor-only leaf must retain U/S = 0, even when
    * its surrounding page-table hierarchy permits user pages.
    */

    /*
     * Global translations are not permitted in process
     * address spaces.
     */
    if (process_mapping &&
        (flags & VMM_MAP_GLOBAL)) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Both addresses must identify the beginning of a 4 KiB page.
     */
    if ((virtual_address & VMM_PAGE_MASK) != 0 ||
        (physical_address & VMM_PAGE_MASK) != 0) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate supported mapping flags.
     */
    const uint64_t allowed_flags =
        VMM_MAP_WRITABLE |
        VMM_MAP_USER |
        VMM_MAP_EXECUTABLE |
        VMM_MAP_GLOBAL |
        VMM_MAP_WRITE_THROUGH |
        VMM_MAP_CACHE_DISABLE;

    if (flags & ~allowed_flags) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
    * The kernel root is never allowed to create user mappings.
    * User mappings must use an independent process address space.
    */
    if (!process_mapping &&
        (flags & VMM_MAP_USER)) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
    * Enforce W^X (Write XOR Execute).
    *
    * A mapping must never be both writable and executable.
    * This prevents writable memory from becoming directly
    * executable through this mapping API.
    */
    if ((flags & VMM_MAP_WRITABLE) &&
        (flags & VMM_MAP_EXECUTABLE)) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Obtain the CPU-supported physical-address mask.
     */
    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Reject physical addresses outside the CPU's supported
     * physical-address width.
     *
     * Physical address zero is not rejected here.
     */
    if (physical_address & ~address_mask) {
        return VMM_STATUS_INVALID_PHYSICAL_ADDRESS;
    }

    /*
     * The public mapping contract defaults to non-executable.
     * This requires both CPU NX support and EFER.NXE enabled.
     */
    if (!(flags & VMM_MAP_EXECUTABLE) &&
        !vmm_nx_enabled) {
        return VMM_STATUS_UNSUPPORTED;
    }

    /*
     * Check whether the virtual address is already mapped.
     *
     * This also detects existing 2 MiB and 1 GiB mappings.
     */
    vmm_mapping_info_t existing_mapping;

    vmm_status_t query_status =
        vmm_query_root(
            root_virtual,
            virtual_address,
            &existing_mapping
        );

    if (query_status == VMM_STATUS_OK) {
        return VMM_STATUS_ALREADY_MAPPED;
    }

    if (query_status != VMM_STATUS_NOT_MAPPED) {
        return query_status;
    }

    /*
     * Extract the four x86_64 page-table indexes.
     */
    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FFULL;

    uint64_t pdpt_index =
        (virtual_address >> 30) & 0x1FFULL;

    uint64_t pd_index =
        (virtual_address >> 21) & 0x1FFULL;

    uint64_t pt_index =
        (virtual_address >> 12) & 0x1FFULL;


    /*
     * Track which intermediate tables this operation creates.
     *
     * These flags are essential for safe rollback.
     */
    bool pml4_created = false;
    bool pdpt_created = false;
    bool pd_created = false;

    volatile uint64_t *pdpt = 0;
    volatile uint64_t *pd = 0;
    volatile uint64_t *pt = 0;

    vmm_status_t status;


    /*
     * LEVEL 4: PML4 -> PDPT
     */
    status = vmm_get_or_create_table(
        &root_virtual[pml4_index],
        flags,
        process_mapping,
        root_physical,
        &pdpt,
        &pml4_created
    );

    if (status != VMM_STATUS_OK) {
        return status;
    }


    /*
     * LEVEL 3: PDPT -> Page Directory
     */
    status = vmm_get_or_create_table(
        &pdpt[pdpt_index],
        flags,
        process_mapping,
        root_physical,
        &pd,
        &pdpt_created
    );

    if (status != VMM_STATUS_OK) {

        /*
        * If the preceding PML4 entry created a new PDPT,
        * remove it because this mapping operation failed.
        *
        * A failed rollback means the page-table hierarchy
        * could not be restored to its original state.
        */
        if (pml4_created &&
            !vmm_rollback_created_table(
                &root_virtual[pml4_index],
                virtual_address,
                root_physical
            )) {

            return VMM_STATUS_INVALID_PAGE_TABLE;
        }

        return status;
    }


    /*
     * LEVEL 2: Page Directory -> Page Table
     */
    status = vmm_get_or_create_table(
        &pd[pd_index],
        flags,
        process_mapping,
        root_physical,
        &pt,
        &pd_created
    );

    if (status != VMM_STATUS_OK) {

        bool rollback_ok = true;

        /*
        * Roll back in reverse hierarchy order.
        */
        if (pdpt_created &&
            !vmm_rollback_created_table(
                &pdpt[pdpt_index],
                virtual_address,
                root_physical
            )) {

            rollback_ok = false;
        }

        if (pml4_created &&
            !vmm_rollback_created_table(
                &root_virtual[pml4_index],
                virtual_address,
                root_physical
            )) {

            rollback_ok = false;
        }

        /*
        * Do not report an ordinary mapping failure when
        * page-table cleanup itself failed.
        */
        if (!rollback_ok) {
            return VMM_STATUS_INVALID_PAGE_TABLE;
        }

        return status;
    }


    /*
     * LEVEL 1: Construct the final 4 KiB PTE.
     *
     * PRESENT is always set.
     */
    uint64_t new_entry =
        (physical_address & address_mask) |
        VMM_PRESENT;

    if (flags & VMM_MAP_WRITABLE) {
        new_entry |= VMM_WRITABLE;
    }

    if (flags & VMM_MAP_USER) {
        new_entry |= VMM_USER;
    }

    if (flags & VMM_MAP_GLOBAL) {
        new_entry |= VMM_GLOBAL;
    }

    if (flags & VMM_MAP_WRITE_THROUGH) {
        new_entry |= VMM_WRITE_THROUGH;
    }

    if (flags & VMM_MAP_CACHE_DISABLE) {
        new_entry |= VMM_CACHE_DISABLE;
    }

    /*
     * Non-executable mappings receive the NX bit.
     */
    if (!(flags & VMM_MAP_EXECUTABLE)) {
        new_entry |= VMM_NO_EXECUTE;
    }


    /*
     * Install the mapping only if the PTE is still empty.
     *
     * Never silently overwrite an existing mapping.
     */
    uint64_t expected_entry = 0;

    if (!__atomic_compare_exchange_n(
            &pt[pt_index],
            &expected_entry,
            new_entry,
            false,
            __ATOMIC_RELEASE,
            __ATOMIC_ACQUIRE)) {

        status = (expected_entry & VMM_PRESENT)
            ? VMM_STATUS_ALREADY_MAPPED
            : VMM_STATUS_INVALID_PAGE_TABLE;

        goto rollback;
    }

    /*
     * Invalidate any stale translation for this virtual address.
     */
    vmm_invalidate_page(virtual_address);

    return VMM_STATUS_OK;


rollback:

    /*
     * Roll back newly created intermediate tables in
     * reverse hierarchy order.
     *
     * The physical data frame remains caller-owned.
     * This function never frees it.
     */
    bool rollback_ok = true;

    if (pd_created &&
        !vmm_rollback_created_table(
            &pd[pd_index],
            virtual_address,
            root_physical
        )) {

        rollback_ok = false;
    }

    if (pdpt_created &&
        !vmm_rollback_created_table(
            &pdpt[pdpt_index],
            virtual_address,
            root_physical
        )) {

        rollback_ok = false;
    }

    if (pml4_created &&
        !vmm_rollback_created_table(
            &root_virtual[pml4_index],
            virtual_address,
            root_physical
        )) {

        rollback_ok = false;
    }

    /*
     * An unsuccessful rollback indicates that the
     * address-space page-table hierarchy needs attention.
     */
    if (!rollback_ok) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}

/*
 * Map a page in the kernel address space.
 *
 * The caller must hold vmm_lock.
 *
 * Upper-half kernel mappings are synchronized into
 * every independently created address space.
 */
static vmm_status_t vmm_map_page_locked(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * Kernel-root mappings must never request user access.
     */
    if (flags & VMM_MAP_USER) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FFULL;

    bool upper_half = pml4_index >= 256;

    /*
     * Validate all process-root copies before modifying
     * an upper-half kernel mapping.
     */
    if (upper_half &&
        !vmm_validate_kernel_pml4_mirror_locked(
            pml4_index
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status =
        vmm_map_page_root_locked(
            vmm_pml4_virtual,
            vmm_pml4_physical,
            virtual_address,
            physical_address,
            flags,
            false
        );

    /*
     * Synchronize even when mapping reports failure.
     *
     * An unsuccessful operation may have encountered a
     * rollback failure after changing the kernel root.
     * Mirroring the final root state prevents stale copies.
     */
    if (upper_half) {
        vmm_sync_kernel_pml4_mirror_locked(
            pml4_index
        );
    }

    return status;
}

vmm_status_t vmm_map_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
) {

    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status = vmm_map_page_locked(
        virtual_address,
        physical_address,
        flags
    );

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}

/*
 * Map one 4 KiB page into an independently owned
 * process address space.
 *
 * The supplied physical frame remains owned by the caller.
 * This function only creates the virtual mapping.
 */
vmm_status_t vmm_address_space_map_page(
    vmm_address_space_t *space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (space == NULL ||
        space == &vmm_kernel_space_object) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status;

    /*
     * Validate the address-space object while holding vmm_lock.
     */
    if (!vmm_address_space_is_registered_unlocked(space) ||
        !space->registered ||
        space->is_kernel ||
        space->root_virtual == NULL ||
        space->root_physical == 0) {

        status = VMM_STATUS_INVALID_ARGUMENT;

    } else if (vmm_test_map_failure_enabled &&
               vmm_test_map_successes_remaining == 0) {

        /*
         * Inject one failure before touching the page tables.
         */
        vmm_test_map_failure_enabled = false;

        status = VMM_STATUS_OUT_OF_MEMORY;

    } else {

        status = vmm_map_page_root_locked(
            space->root_virtual,
            space->root_physical,
            virtual_address,
            physical_address,
            flags,
            true
        );

        /*
         * Count only mappings that actually succeeded.
         *
         * A collision or an internal page-table allocation
         * failure does not consume a successful-map count.
         */
        if (status == VMM_STATUS_OK &&
            vmm_test_map_failure_enabled) {

            vmm_test_map_successes_remaining--;
        }
    }

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        interrupt_flags
    );

    return status;
}

/*
 * Map a contiguous range of physical 4 KiB frames.
 *
 * The operation is transactional:
 * if any page fails to map, all mappings created by this
 * operation are removed.
 *
 * Physical data frames always remain caller-owned.
 *
 * The VMM lock protects preflight, mapping and rollback
 * as one transaction.
 */
vmm_status_t vmm_map_range(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t page_count,
    uint64_t flags
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (page_count == 0) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if ((virtual_address & VMM_PAGE_MASK) ||
        (physical_address & VMM_PAGE_MASK)) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate supported mapping flags.
     */
    const uint64_t allowed_flags =
        VMM_MAP_WRITABLE |
        VMM_MAP_EXECUTABLE |
        VMM_MAP_GLOBAL |
        VMM_MAP_WRITE_THROUGH |
        VMM_MAP_CACHE_DISABLE;

    if (flags & ~allowed_flags) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Enforce W^X.
     */
    if ((flags & VMM_MAP_WRITABLE) &&
        (flags & VMM_MAP_EXECUTABLE)) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if (!(flags & VMM_MAP_EXECUTABLE) &&
        !vmm_nx_enabled) {
        return VMM_STATUS_UNSUPPORTED;
    }

    /*
     * Validate range arithmetic before mapping anything.
     */
    if (page_count - 1 >
        UINT64_MAX / VMM_PAGE_SIZE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t maximum_offset =
        (page_count - 1) * VMM_PAGE_SIZE;

    if (virtual_address > UINT64_MAX - maximum_offset ||
        physical_address > UINT64_MAX - maximum_offset) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t last_virtual =
        virtual_address + maximum_offset;

    uint64_t last_physical =
        physical_address + maximum_offset;

    if (!vmm_is_canonical(last_virtual)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * Ensure the range stays within one canonical region.
     */
    if (((virtual_address >> 47) & 1ULL) !=
        ((last_virtual >> 47) & 1ULL)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if ((virtual_address & (1ULL << 47)) !=
        (last_virtual & (1ULL << 47))) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * Validate the complete physical range.
     */
    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    if ((physical_address & ~address_mask) ||
        (last_physical & ~address_mask)) {
        return VMM_STATUS_INVALID_PHYSICAL_ADDRESS;
    }

    /*
     * Acquire the VMM lock once for the complete transaction.
     */
    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status = VMM_STATUS_OK;

    /*
     * Phase 1: preflight every virtual page.
     *
     * Use the internal query helper because the lock
     * is already held.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t current_virtual =
            virtual_address + i * VMM_PAGE_SIZE;

        vmm_mapping_info_t existing_mapping;

        vmm_status_t query_status =
            vmm_query_root(
                vmm_pml4_virtual,
                current_virtual,
                &existing_mapping
            );

        if (query_status == VMM_STATUS_OK) {
            status = VMM_STATUS_ALREADY_MAPPED;
            goto unlock;
        }

        if (query_status != VMM_STATUS_NOT_MAPPED) {
            status = query_status;
            goto unlock;
        }
    }

    /*
     * Phase 2: install the mappings.
     */
    uint64_t mapped_pages = 0;

    for (; mapped_pages < page_count; mapped_pages++) {

        uint64_t current_virtual =
            virtual_address + mapped_pages * VMM_PAGE_SIZE;

        uint64_t current_physical =
            physical_address + mapped_pages * VMM_PAGE_SIZE;

        status = vmm_map_page_locked(
            current_virtual,
            current_physical,
            flags
        );

        if (status != VMM_STATUS_OK) {
            break;
        }
    }

    /*
     * Phase 3: rollback if mapping failed.
     */
    if (status != VMM_STATUS_OK) {

        while (mapped_pages > 0) {

            mapped_pages--;

            uint64_t rollback_virtual =
                virtual_address +
                mapped_pages * VMM_PAGE_SIZE;

            uint64_t expected_physical =
                physical_address +
                mapped_pages * VMM_PAGE_SIZE;

            uint64_t removed_physical = 0;

            vmm_status_t rollback_status =
                vmm_unmap_page_locked(
                    rollback_virtual,
                    &removed_physical
                );

            /*
             * Stop if rollback cannot be verified.
             */
            if (rollback_status != VMM_STATUS_OK ||
                removed_physical != expected_physical) {

                status = VMM_STATUS_INVALID_PAGE_TABLE;
                break;
            }
        }
    }

unlock:

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}

/*
 * Remove one 4 KiB mapping from the active address space.
 *
 * The physical data frame is returned to the caller.
 * VMM never automatically frees the data frame.
 *
 * Empty intermediate page tables are reclaimed only when
 * they are registered as owned by VMM.
 *
 * Huge-page mappings must be split before unmapping.
 */
static vmm_status_t vmm_unmap_page_root_locked(
    volatile uint64_t *root_virtual,
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t *physical_address
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    /*
     * Validate the page-table root.
     */
    if (root_virtual == NULL ||
        root_physical == 0 ||
        (root_physical & VMM_PAGE_MASK) != 0) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate arguments.
     */
    if (physical_address == 0) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (virtual_address & VMM_PAGE_MASK) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    *physical_address = 0;

    uint64_t address_mask =
        vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Extract the four page-table indexes.
     */
    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FFULL;

    uint64_t pdpt_index =
        (virtual_address >> 30) & 0x1FFULL;

    uint64_t pd_index =
        (virtual_address >> 21) & 0x1FFULL;

    uint64_t pt_index =
        (virtual_address >> 12) & 0x1FFULL;


    /*
     * LEVEL 4: PML4 -> PDPT
     */
    uint64_t pml4_entry =
        __atomic_load_n(
            &root_virtual[pml4_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pml4_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (pml4_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    volatile uint64_t *pdpt =
        vmm_get_table(pml4_entry & address_mask);

    if (pdpt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }


    /*
     * LEVEL 3: PDPT -> Page Directory
     */
    uint64_t pdpt_entry =
        __atomic_load_n(
            &pdpt[pdpt_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pdpt_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    /*
     * A 1 GiB huge mapping cannot be removed using
     * this 4 KiB unmapping interface.
     */
    if (pdpt_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pd =
        vmm_get_table(pdpt_entry & address_mask);

    if (pd == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }


    /*
     * LEVEL 2: Page Directory -> Page Table
     */
    uint64_t pd_entry =
        __atomic_load_n(
            &pd[pd_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pd_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    /*
     * A 2 MiB huge mapping must be split first.
     */
    if (pd_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pt =
        vmm_get_table(pd_entry & address_mask);

    if (pt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }


    /*
     * LEVEL 1: Locate the 4 KiB page-table entry.
     */
    uint64_t old_entry =
        __atomic_load_n(
            &pt[pt_index],
            __ATOMIC_ACQUIRE
        );

    if (!(old_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    /*
     * Extract the physical data-frame address.
     *
     * Do this before clearing the entry because the
     * caller needs the address to manage the frame.
     */
    uint64_t unmapped_physical =
        old_entry & address_mask;

    /*
     * Atomically remove the mapping.
     *
     * If the entry changed after our initial read,
     * do not overwrite the newer entry.
     */
    uint64_t expected_entry = old_entry;

    if (!__atomic_compare_exchange_n(
            &pt[pt_index],
            &expected_entry,
            0,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Invalidate the virtual address in the local TLB.
     *
     * This prevents the CPU from continuing to use
     * the old cached translation.
     */
    vmm_invalidate_page(virtual_address);

    /*
     * The mapping has now been removed successfully.
     * Return the physical frame to the caller.
     */
    *physical_address = unmapped_physical;


    /*
     * Reclaim empty VMM-owned intermediate tables.
     *
     * Reclamation proceeds from the lowest level upward:
     *
     * Page Table -> Page Directory -> PDPT
     *
     * A table is reclaimed only if:
     * - It belongs to VMM.
     * - All 512 entries are empty.
     * - Its parent entry can be atomically detached.
     *
     * Existing Limine-owned or kernel-owned tables
     * are never reclaimed by this operation.
     *
     * Reclamation failure does not undo the successful
     * unmapping. An unused table may safely remain allocated.
     */

    vmm_rollback_created_table(
        &pd[pd_index],
        virtual_address,
        root_physical
    );

    vmm_rollback_created_table(
        &pdpt[pdpt_index],
        virtual_address,
        root_physical
    );

    vmm_rollback_created_table(
        &root_virtual[pml4_index],
        virtual_address,
        root_physical
    );

    return VMM_STATUS_OK;
}

/*
 * Remove a mapping from the kernel address space.
 *
 * The caller must hold vmm_lock.
 *
 * Upper-half kernel PML4 changes are propagated into
 * every independently created address space.
 */
static vmm_status_t vmm_unmap_page_locked(
    uint64_t virtual_address,
    uint64_t *physical_address
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (physical_address == NULL) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FFULL;

    bool upper_half = pml4_index >= 256;

    /*
     * Validate existing kernel/process root consistency
     * before removing anything.
     */
    if (upper_half &&
        !vmm_validate_kernel_pml4_mirror_locked(
            pml4_index
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status =
        vmm_unmap_page_root_locked(
            vmm_pml4_virtual,
            vmm_pml4_physical,
            virtual_address,
            physical_address
        );

    /*
     * The internal unmapper may have detached the kernel
     * PML4 branch while reclaiming empty intermediate tables.
     *
     * Propagate its final state regardless of status.
     */
    if (upper_half) {
        vmm_sync_kernel_pml4_mirror_locked(
            pml4_index
        );
    }

    return status;
}

vmm_status_t vmm_unmap_page(
    uint64_t virtual_address,
    uint64_t *physical_address
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status = vmm_unmap_page_locked(
        virtual_address,
        physical_address
    );

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}


/*
 * Remove one 4 KiB mapping from an independently owned
 * process address space.
 *
 * The physical data frame is returned to the caller.
 * VMM never automatically frees the data frame.
 *
 * Only lower-half user mappings may be removed through
 * this interface.
 */
vmm_status_t vmm_address_space_unmap_page(
    vmm_address_space_t *space,
    uint64_t virtual_address,
    uint64_t *physical_address
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (space == NULL ||
        space == &vmm_kernel_space_object ||
        physical_address == NULL) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    *physical_address = 0;

    /*
     * Process address spaces may only unmap lower-half addresses.
     */
    if (virtual_address >= 0x0000800000000000ULL) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status;

    /*
     * Validate the address-space object while holding vmm_lock.
     */
    if (!vmm_address_space_is_registered_unlocked(space) ||
        !space->registered ||
        space->is_kernel ||
        space->root_virtual == NULL ||
        space->root_physical == 0) {

        status = VMM_STATUS_INVALID_ARGUMENT;

    } else if (vmm_test_unmap_failure_enabled &&
               virtual_address ==
                   vmm_test_unmap_virtual_address) {

        /*
         * Consume the one-shot failure without modifying the PTE.
         *
         * INVALID_PAGE_TABLE is deliberately used as a transient
         * VMM operation failure for this test hook.
         */
        vmm_test_unmap_failure_enabled = false;

        status = VMM_STATUS_INVALID_PAGE_TABLE;

    } else if (vmm_test_unmap_next_failure_enabled) {

        /*
         * Consume the one-shot failure without
         * modifying the page-table entry.
         */
        vmm_test_unmap_next_failure_enabled = false;

        status = VMM_STATUS_INVALID_PAGE_TABLE;

    } else {

        status = vmm_unmap_page_root_locked(
            space->root_virtual,
            space->root_physical,
            virtual_address,
            physical_address
        );
    }

    raw_spinlock_unlock_irqrestore(
        &vmm_lock,
        interrupt_flags
    );

    return status;
}

/*
 * Remove a contiguous range of 4 KiB mappings from the
 * kernel address space.
 *
 * The operation is protected by one VMM lock.
 *
 * Physical data frames remain caller-owned and are returned
 * through physical_addresses.
 *
 * Empty VMM-owned intermediate page tables are reclaimed
 * by vmm_unmap_page_root_locked().
 */
vmm_status_t vmm_unmap_range(
    uint64_t virtual_address,
    uint64_t page_count,
    uint64_t *physical_addresses
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    /*
     * Validate arguments.
     */
    if (page_count == 0 || physical_addresses == NULL) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (virtual_address & VMM_PAGE_MASK) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate range arithmetic.
     */
    if (page_count - 1 > UINT64_MAX / VMM_PAGE_SIZE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t maximum_offset =
        (page_count - 1) * VMM_PAGE_SIZE;

    if (virtual_address > UINT64_MAX - maximum_offset) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t last_virtual =
        virtual_address + maximum_offset;

    if (!vmm_is_canonical(last_virtual)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * Ensure the range does not cross canonical halves.
     */
    if (((virtual_address >> 47) & 1ULL) !=
        ((last_virtual >> 47) & 1ULL)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * Acquire the lock once for the complete operation.
     */
    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status = VMM_STATUS_OK;

    /*
     * PHASE 1: PREFLIGHT
     *
     * Verify every mapping before removing any mapping.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t current_virtual =
            virtual_address + i * VMM_PAGE_SIZE;

        vmm_mapping_info_t mapping;

        status = vmm_query_root(
            vmm_pml4_virtual,
            current_virtual,
            &mapping
        );

        if (status != VMM_STATUS_OK) {
            goto unlock;
        }

        /*
         * This operation only removes individual 4 KiB
         * mappings. Huge pages must be split first.
         */
        if (mapping.page_size != VMM_PAGE_SIZE ||
            mapping.huge_page) {

            status = VMM_STATUS_INVALID_ARGUMENT;
            goto unlock;
        }
    }

    /*
     * PHASE 2: REMOVE MAPPINGS
     *
     * The VMM lock remains held, so another VMM operation
     * cannot change the mappings between preflight and
     * removal.
     *
     * The internal helper also:
     * - Clears the PTE atomically.
     * - Invalidates the local TLB entry.
     * - Reclaims empty VMM-owned intermediate tables.
     *
     * It does not free the physical data frame.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t current_virtual =
            virtual_address + i * VMM_PAGE_SIZE;

        uint64_t removed_physical = 0;

        status = vmm_unmap_page_locked(
            current_virtual,
            &removed_physical
        );

        if (status != VMM_STATUS_OK) {
            goto unlock;
        }

        physical_addresses[i] = removed_physical;
    }

unlock:

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}


/*
 * Split an existing 2 MiB huge-page mapping into 512 individual
 * 4 KiB page mappings.
 *
 * The original physical mappings and supported leaf attributes
 * are preserved.
 */
static vmm_status_t vmm_split_2m_locked(
    uint64_t virtual_address
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (virtual_address & (VMM_PAGE_2M - 1ULL)) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Calculate the page-table indexes.
     */
    uint64_t pml4_index =
        (virtual_address >> 39) & 0x1FFULL;

    uint64_t pdpt_index =
        (virtual_address >> 30) & 0x1FFULL;

    uint64_t pd_index =
        (virtual_address >> 21) & 0x1FFULL;

    /*
     * Level 4: PML4.
     */
    uint64_t pml4_entry =
        __atomic_load_n(
            &vmm_pml4_virtual[pml4_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pml4_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (pml4_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    volatile uint64_t *pdpt =
        vmm_get_table(
            pml4_entry & vmm_get_physical_address_mask()
        );

    if (pdpt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Level 3: PDPT.
     */
    uint64_t pdpt_entry =
        __atomic_load_n(
            &pdpt[pdpt_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pdpt_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    /*
     * This operation supports 2 MiB pages, not 1 GiB pages.
     */
    if (pdpt_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pd =
        vmm_get_table(
            pdpt_entry & vmm_get_physical_address_mask()
        );

    if (pd == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Level 2: Page Directory.
     */
    uint64_t huge_entry =
        __atomic_load_n(
            &pd[pd_index],
            __ATOMIC_ACQUIRE
        );

    if (!(huge_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    /*
     * If PS=0, this entry already points to a 4 KiB page table.
     */
    if (!(huge_entry & VMM_HUGE_PAGE)) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Reject physical address bits outside the CPU-supported width.
     *
     * Bits 21-51 contain the 2 MiB physical base address.
     */
    uint64_t address_mask =
        vmm_get_physical_address_mask();

    uint64_t physical_field_mask =
        ((1ULL << 52) - 1ULL) &
        ~(VMM_PAGE_2M - 1ULL);

    if (huge_entry & physical_field_mask & ~address_mask) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

      /*
     * Allocate a new zero-initialized page-table frame.
     */
    uint64_t table_physical = 0;

    volatile uint64_t *new_table =
        vmm_allocate_table_frame(&table_physical);

    if (new_table == 0) {
        return VMM_STATUS_OUT_OF_MEMORY;
    }

    /*
     * Populate all 512 individual 4 KiB mappings.
     */
    if (!vmm_split_2m_mapping(huge_entry, new_table)) {

        pmm_free_frame(table_physical);

        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Register ownership before publishing the new page table.
     */
    if (!vmm_register_owned_table(
        table_physical,
        vmm_pml4_physical
    )) {

        pmm_free_frame(table_physical);

        return VMM_STATUS_OUT_OF_MEMORY;
    }

    /*
     * Replace the original huge-page entry.
     *
     * The CPU may independently update the Accessed and Dirty
     * bits in a page-directory entry. If that happens, our CAS
     * must retry using the latest entry rather than discarding
     * those hardware updates.
     */
    for (;;) {

        uint64_t new_entry =
            vmm_create_table_entry(
                huge_entry,
                table_physical
            );

        uint64_t expected_entry = huge_entry;

        if (__atomic_compare_exchange_n(
                &pd[pd_index],
                &expected_entry,
                new_entry,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {

            break;
        }

        /*
         * CAS failed.
         *
         * Check whether the only changes were hardware-managed
         * Accessed and Dirty bits.
         */
        if ((expected_entry &
                ~(VMM_ACCESSED | VMM_DIRTY)) !=
            (huge_entry &
                ~(VMM_ACCESSED | VMM_DIRTY))) {

            /*
             * An unexpected page-directory modification occurred.
             * Do not publish our replacement.
             */
            if (!vmm_unregister_owned_table(
                table_physical,
                vmm_pml4_physical
            )) {
                /*
                 * Keep the frame allocated if ownership removal
                 * fails. Freeing it could leave a dangling registry
                 * entry.
                 */
                return VMM_STATUS_INVALID_PAGE_TABLE;
            }

            pmm_free_frame(table_physical);

            return VMM_STATUS_INVALID_PAGE_TABLE;
        }

        /*
         * Only Accessed/Dirty bits changed.
         *
         * Preserve their latest values in the new 4 KiB mappings,
         * then retry the replacement.
         */
        huge_entry = expected_entry;

        if (!vmm_split_2m_mapping(huge_entry, new_table)) {

            if (!vmm_unregister_owned_table(
                table_physical,
                vmm_pml4_physical
            )) {
                return VMM_STATUS_INVALID_PAGE_TABLE;
            }

            pmm_free_frame(table_physical);

            return VMM_STATUS_INVALID_PAGE_TABLE;
        }
    }

    /*
     * Remove stale translations from the TLB.
     */
    vmm_invalidate_2m_range(virtual_address);

    return VMM_STATUS_OK;
}

/*
 * Split a 2 MiB mapping into 512 individual 4 KiB mappings.
 *
 * The VMM lock protects page-table modifications and the
 * owned-page-table registry during the entire operation.
 */
vmm_status_t vmm_split_2m(uint64_t virtual_address) {


    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status =
        vmm_split_2m_locked(virtual_address);

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}

/*
 * Resolve the leaf PTE for an existing 4 KiB mapping.
 *
 * The helper does not allocate or modify page tables.
 *
 * ancestor_flags receives the effective permission bits
 * inherited from the PML4, PDPT and PD entries.
 */
static vmm_status_t vmm_get_protection_pte(
    uint64_t virtual_address,
    volatile uint64_t **pte_out,
    uint64_t *ancestor_flags
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (pte_out == 0 || ancestor_flags == 0) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    *pte_out = 0;
    *ancestor_flags = 0;

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (virtual_address & VMM_PAGE_MASK) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t address_mask = vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    uint64_t pml4_index = (virtual_address >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_address >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_address >> 12) & 0x1FF;

    /*
     * PML4.
     */
    uint64_t pml4_entry =
        __atomic_load_n(
            &vmm_pml4_virtual[pml4_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pml4_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (pml4_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    volatile uint64_t *pdpt =
        vmm_get_table(pml4_entry & address_mask);

    if (pdpt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * PDPT.
     */
    uint64_t pdpt_entry =
        __atomic_load_n(
            &pdpt[pdpt_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pdpt_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (pdpt_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pd =
        vmm_get_table(pdpt_entry & address_mask);

    if (pd == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Page directory.
     */
    uint64_t pd_entry =
        __atomic_load_n(
            &pd[pd_index],
            __ATOMIC_ACQUIRE
        );

    if (!(pd_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (pd_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pt =
        vmm_get_table(pd_entry & address_mask);

    if (pt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Calculate inherited permissions.
     */
    uint64_t inherited = VMM_PRESENT;

    if ((pml4_entry & VMM_WRITABLE) &&
        (pdpt_entry & VMM_WRITABLE) &&
        (pd_entry & VMM_WRITABLE)) {

        inherited |= VMM_WRITABLE;
    }

    if ((pml4_entry & VMM_USER) &&
        (pdpt_entry & VMM_USER) &&
        (pd_entry & VMM_USER)) {

        inherited |= VMM_USER;
    }

    if (vmm_nx_enabled &&
        ((pml4_entry |
          pdpt_entry |
          pd_entry) & VMM_NO_EXECUTE)) {

        inherited |= VMM_NO_EXECUTE;
    }

    /*
     * Resolve the final PTE.
     */
    volatile uint64_t *entry_address =
        &pt[pt_index];

    uint64_t leaf_entry =
        __atomic_load_n(
            entry_address,
            __ATOMIC_ACQUIRE
        );

    if (!(leaf_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    *pte_out = &pt[pt_index];
    *ancestor_flags = inherited;

    return VMM_STATUS_OK;
}


/*
 * Validate a requested protection configuration.
 */
static vmm_status_t vmm_validate_protection_flags(
    uint64_t flags
) {

    const uint64_t allowed_flags =
        VMM_MAP_WRITABLE |
        VMM_MAP_EXECUTABLE;

    if (flags & ~allowed_flags) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Enforce write XOR execute.
     */
    if ((flags & VMM_MAP_WRITABLE) &&
        (flags & VMM_MAP_EXECUTABLE)) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * NX must be supported and enabled to guarantee
     * non-executable mappings.
     */
    if (!(flags & VMM_MAP_EXECUTABLE) &&
        !vmm_nx_enabled) {

        return VMM_STATUS_UNSUPPORTED;
    }

    return VMM_STATUS_OK;
}


/*
 * Change permissions on one existing 4 KiB mapping.
 */
static vmm_status_t vmm_protect_page_locked(
    uint64_t virtual_address,
    uint64_t flags
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (virtual_address & VMM_PAGE_MASK) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    vmm_status_t status =
        vmm_validate_protection_flags(flags);

    if (status != VMM_STATUS_OK) {
        return status;
    }

    volatile uint64_t *pte = 0;
    uint64_t ancestor_flags = 0;

    status = vmm_get_protection_pte(
        virtual_address,
        &pte,
        &ancestor_flags
    );

    if (status != VMM_STATUS_OK) {
        return status;
    }

    /*
     * Parent restrictions cannot be bypassed by changing
     * only the final PTE.
     */
    if ((flags & VMM_MAP_WRITABLE) &&
        !(ancestor_flags & VMM_WRITABLE)) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if ((flags & VMM_MAP_USER) &&
        !(ancestor_flags & VMM_USER)) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if ((flags & VMM_MAP_EXECUTABLE) &&
        (ancestor_flags & VMM_NO_EXECUTE)) {

        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Atomically update the leaf entry.
     *
     * Preserve:
     * - Physical frame address
     * - Cache attributes
     * - Global bit
     * - Accessed and dirty bits
     * - PAT and software-defined bits
     *
     * Change only:
     * - Writable
     * - User
     * - NX
     */
    uint64_t old_entry =
        __atomic_load_n(pte, __ATOMIC_ACQUIRE);

    for (;;) {

        if (!(old_entry & VMM_PRESENT)) {
            return VMM_STATUS_NOT_MAPPED;
        }

        uint64_t new_entry = old_entry;

        new_entry &= ~(
            VMM_WRITABLE |
            VMM_USER |
            VMM_NO_EXECUTE
        );

        if (flags & VMM_MAP_WRITABLE) {
            new_entry |= VMM_WRITABLE;
        }

        if (flags & VMM_MAP_USER) {
            new_entry |= VMM_USER;
        }

        if (!(flags & VMM_MAP_EXECUTABLE)) {
            new_entry |= VMM_NO_EXECUTE;
        }

        if (new_entry == old_entry) {
            return VMM_STATUS_OK;
        }

        uint64_t expected_entry = old_entry;

        if (__atomic_compare_exchange_n(
                pte,
                &expected_entry,
                new_entry,
                false,
                __ATOMIC_RELEASE,
                __ATOMIC_ACQUIRE
            )) {

            break;
        }

        old_entry = expected_entry;
    }

    /*
     * Remove the old translation and permissions
     * from the local CPU's TLB.
     */
    vmm_invalidate_page(virtual_address);

    return VMM_STATUS_OK;
}

/*
 * Change permissions on one existing 4 KiB mapping.
 *
 * The VMM lock protects page-table traversal and modification.
 */
vmm_status_t vmm_protect_page(
    uint64_t virtual_address,
    uint64_t flags
) {

    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status = vmm_protect_page_locked(
        virtual_address,
        flags
    );

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}


/*
 * Change permissions across a contiguous range of 4 KiB mappings.
 *
 * The VMM lock protects preflight and all permission changes
 * as one operation.
 *
 * All pages are validated before any permission is modified.
 */
vmm_status_t vmm_protect_range(
    uint64_t virtual_address,
    uint64_t page_count,
    uint64_t flags
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (page_count == 0) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (virtual_address & VMM_PAGE_MASK) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Validate requested permissions.
     */
    vmm_status_t status =
        vmm_validate_protection_flags(flags);

    if (status != VMM_STATUS_OK) {
        return status;
    }

    /*
     * Validate range arithmetic.
     */
    if (page_count - 1 >
        UINT64_MAX / VMM_PAGE_SIZE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t maximum_offset =
        (page_count - 1) * VMM_PAGE_SIZE;

    if (virtual_address >
        UINT64_MAX - maximum_offset) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t last_virtual =
        virtual_address + maximum_offset;

    if (!vmm_is_canonical(last_virtual)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * The range must remain within one canonical half.
     */
    if (((virtual_address >> 47) & 1ULL) !=
        ((last_virtual >> 47) & 1ULL)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    /*
     * Acquire the VMM lock once for the complete operation.
     */
    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * PHASE 1: PREFLIGHT
     *
     * Validate every page and its ancestor permissions
     * before modifying any PTE.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t current_virtual =
            virtual_address + i * VMM_PAGE_SIZE;

        volatile uint64_t *pte = NULL;
        uint64_t ancestor_flags = 0;

        status = vmm_get_protection_pte(
            current_virtual,
            &pte,
            &ancestor_flags
        );

        if (status != VMM_STATUS_OK) {
            goto unlock;
        }

        /*
         * A 4 KiB leaf mapping is required.
         */
        uint64_t entry =
            __atomic_load_n(pte, __ATOMIC_ACQUIRE);

        if (!(entry & VMM_PRESENT)) {
            status = VMM_STATUS_NOT_MAPPED;
            goto unlock;
        }

        /*
         * Parent restrictions cannot be bypassed
         * by modifying only the leaf PTE.
         */
        if ((flags & VMM_MAP_WRITABLE) &&
            !(ancestor_flags & VMM_WRITABLE)) {
            status = VMM_STATUS_INVALID_ARGUMENT;
            goto unlock;
        }

        if ((flags & VMM_MAP_USER) &&
            !(ancestor_flags & VMM_USER)) {
            status = VMM_STATUS_INVALID_ARGUMENT;
            goto unlock;
        }

        if ((flags & VMM_MAP_EXECUTABLE) &&
            (ancestor_flags & VMM_NO_EXECUTE)) {
            status = VMM_STATUS_INVALID_ARGUMENT;
            goto unlock;
        }
    }

    /*
     * PHASE 2: APPLY PERMISSIONS
     *
     * Use the internal helper because vmm_lock is already held.
     * Do not call the public vmm_protect_page() wrapper here.
     */
    for (uint64_t i = 0; i < page_count; i++) {

        uint64_t current_virtual =
            virtual_address + i * VMM_PAGE_SIZE;

        status = vmm_protect_page_locked(
            current_virtual,
            flags
        );

        if (status != VMM_STATUS_OK) {
            /*
             * Preflight succeeded, so an error here indicates
             * an unexpected page-table inconsistency.
             *
             * Do not claim that earlier changes were rolled back.
             */
            status = VMM_STATUS_INVALID_PAGE_TABLE;
            goto unlock;
        }
    }

    status = VMM_STATUS_OK;

unlock:

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}


/*
 * Change the executable permission of an existing 4 KiB mapping.
 *
 * executable = true:
 *     Allow instruction execution by clearing NX.
 *
 * executable = false:
 *     Prevent instruction execution by setting NX.
 *
 * This function does not support 1 GiB or 2 MiB huge mappings.
 * Split a 2 MiB mapping first using vmm_split_2m().
 */
static vmm_status_t vmm_set_page_executable_locked(
    uint64_t virtual_address,
    bool executable
) {
    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    if (!vmm_is_canonical(virtual_address)) {
        return VMM_STATUS_NON_CANONICAL;
    }

    if (virtual_address & VMM_PAGE_MASK) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * NX cannot safely be set unless EFER.NXE is enabled.
     */
    if (!executable && !vmm_nx_enabled) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    uint64_t address_mask = vmm_get_physical_address_mask();

    if (address_mask == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Extract the four page-table indexes.
     */
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FF;
    uint64_t pd_index   = (virtual_address >> 21) & 0x1FF;
    uint64_t pt_index   = (virtual_address >> 12) & 0x1FF;

    /*
     * Level 4: PML4.
     */
    uint64_t pml4_entry = vmm_pml4_virtual[pml4_index];

    if (!(pml4_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (pml4_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    if (executable &&
        vmm_nx_enabled &&
        (pml4_entry & VMM_NO_EXECUTE)) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pdpt =
        vmm_get_table(pml4_entry & address_mask);

    if (pdpt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Level 3: Page-directory pointer table.
     */
    uint64_t pdpt_entry = pdpt[pdpt_index];

    if (!(pdpt_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (executable &&
        vmm_nx_enabled &&
        (pdpt_entry & VMM_NO_EXECUTE)) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * A 1 GiB huge mapping cannot be modified by this API.
     */
    if (pdpt_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pd =
        vmm_get_table(pdpt_entry & address_mask);

    if (pd == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Level 2: Page directory.
     */
    uint64_t pd_entry = pd[pd_index];

    if (!(pd_entry & VMM_PRESENT)) {
        return VMM_STATUS_NOT_MAPPED;
    }

    if (executable &&
        vmm_nx_enabled &&
        (pd_entry & VMM_NO_EXECUTE)) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    /*
     * A 2 MiB huge mapping must be split first.
     */
    if (pd_entry & VMM_HUGE_PAGE) {
        return VMM_STATUS_INVALID_ARGUMENT;
    }

    volatile uint64_t *pt =
        vmm_get_table(pd_entry & address_mask);

    if (pt == 0) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    /*
     * Atomically update the NX bit while preserving all
     * other PTE attributes, including hardware-managed bits.
     *
     * Enforce W^X:
     * An executable mapping must not be writable.
     */
    uint64_t old_entry =
        __atomic_load_n(&pt[pt_index], __ATOMIC_ACQUIRE);

    for (;;) {

        if (!(old_entry & VMM_PRESENT)) {
            return VMM_STATUS_NOT_MAPPED;
        }

        if (executable && (old_entry & VMM_WRITABLE)) {
            return VMM_STATUS_INVALID_ARGUMENT;
        }

        uint64_t new_entry = old_entry;

        if (executable) {
            new_entry &= ~VMM_NO_EXECUTE;
        } else {
            new_entry |= VMM_NO_EXECUTE;
        }

        /*
         * No change means no TLB invalidation is needed.
         */
        if (new_entry == old_entry) {
            return VMM_STATUS_OK;
        }

        uint64_t expected_entry = old_entry;

        if (__atomic_compare_exchange_n(
                &pt[pt_index],
                &expected_entry,
                new_entry,
                false,
                __ATOMIC_RELEASE,
                __ATOMIC_ACQUIRE
            )) {

            break;
        }

        /*
         * A hardware-managed bit or another writer changed
         * the entry. Retry using the newly observed value.
         */
        old_entry = expected_entry;
    }

    /*
     * Invalidate the old translation on the local CPU.
     */
    vmm_invalidate_page(virtual_address);

    return VMM_STATUS_OK;
}

/*
 * Public interface for changing page executable permission.
 *
 * The VMM lock protects page-table traversal and modification.
 */
vmm_status_t vmm_set_page_executable(
    uint64_t virtual_address,
    bool executable
) {

    if (!vmm_initialized) {
        return VMM_STATUS_NOT_INITIALIZED;
    }

    uint64_t interrupt_flags = 0;

    if (!raw_spinlock_lock_irqsave(
            &vmm_lock,
            &interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    vmm_status_t status = vmm_set_page_executable_locked(
        virtual_address,
        executable
    );

    if (!raw_spinlock_unlock_irqrestore(
            &vmm_lock,
            interrupt_flags
        )) {
        return VMM_STATUS_INVALID_PAGE_TABLE;
    }

    return status;
}


bool vmm_nx_is_supported(void) {
    return vmm_nx_supported;
}

bool vmm_nx_is_enabled(void) {
    return vmm_nx_enabled;
}
