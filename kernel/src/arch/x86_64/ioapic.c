#include "ioapic.h"

#include <stddef.h>

#include "lib/memory.h"
#include "mm/vmm.h"
#include "interrupts/irq.h"
#include "arch/x86_64/lapic.h"

#define IOAPIC_PAGE_SIZE       0x1000ULL
#define IOAPIC_PAGE_MASK       (IOAPIC_PAGE_SIZE - 1ULL)

/*
 * Dedicated virtual address region.
 *
 * Separate from the LAPIC and heap virtual regions.
 */
#define IOAPIC_VIRTUAL_BASE    0xFFFFFD0000000000ULL

#define IOAPIC_REG_SELECT     0x00
#define IOAPIC_REG_WINDOW     0x10

#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VERSION    0x01

#define IOAPIC_REG_REDIR_BASE 0x10
#define IOAPIC_MAX_REDIRECTION_ENTRIES 120U

#define IOAPIC_REDIR_MASK     (1U << 16)
#define IOAPIC_REDIR_LOW_MASK 0x0001FFFFU

#define IOAPIC_REDIR_POLARITY_LOW (1U << 13)
#define IOAPIC_REDIR_TRIGGER_LEVEL (1U << 15)

#define IOAPIC_REDIR_FIXED_DELIVERY 0U
#define IOAPIC_REDIR_PHYSICAL_DEST  0U

#define IOAPIC_PAT_ENTRY_UC 3U
#define IOAPIC_PAT_TYPE_UC  0U

#define IOAPIC_MSR_PAT 0x277U

struct ioapic_controller {
    struct ioapic_controller_info info;
    volatile uint32_t *registers;
    bool initialized;
    bool mapping_created;
    bool route_configured[256];
};

static struct ioapic_controller controllers[IOAPIC_MAX_CONTROLLERS];

static uint32_t controller_count;
static bool ioapic_initialized;

/*
 * A vector is marked when at least one configured I/O APIC
 * route uses it.
 */
static bool vector_is_ioapic[256];

/* MADT data is retained for ISA interrupt overrides. */
static struct acpi_madt_info saved_madt;

/*
 * Current implementation is single-CPU.
 *
 * CLI protects the selector/window pair from maskable
 * interrupt re-entry. SMP will require a shared spinlock.
 */
static uint64_t ioapic_save_disable(void) {

    uint64_t flags;

    asm volatile (
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void ioapic_restore_interrupts(uint64_t flags) {

    if (flags & (1ULL << 9)) {
        asm volatile ("sti" ::: "memory");
    } else {
        asm volatile ("cli" ::: "memory");
    }
}

static uint64_t ioapic_read_msr(uint32_t msr) {

    uint32_t low;
    uint32_t high;

    asm volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );

    return ((uint64_t)high << 32) | low;
}

static bool ioapic_pat_supports_uc(void) {

    uint64_t pat = ioapic_read_msr(IOAPIC_MSR_PAT);

    uint8_t entry =
        (uint8_t)(pat >> (IOAPIC_PAT_ENTRY_UC * 8U));

    return entry == IOAPIC_PAT_TYPE_UC;
}

static bool ioapic_controller_range_valid(
    uint32_t base,
    uint32_t count
) {
    if (count == 0) {
        return false;
    }

    if (base > UINT32_MAX - (count - 1U)) {
        return false;
    }

    return true;
}

static bool ioapic_find_controller(
    uint32_t gsi,
    uint32_t *controller_index,
    uint32_t *redirection_index
) {
    if (!controller_index || !redirection_index) {
        return false;
    }

    for (uint32_t i = 0; i < controller_count; i++) {

        const struct ioapic_controller_info *info =
            &controllers[i].info;

        if (gsi < info->gsi_base) {
            continue;
        }

        uint32_t index = gsi - info->gsi_base;

        if (index >= info->redirection_count) {
            continue;
        }

        *controller_index = i;
        *redirection_index = index;

        return true;
    }

    return false;
}

static uint32_t ioapic_read_register(
    struct ioapic_controller *controller,
    uint8_t reg
) {
    uint64_t flags = ioapic_save_disable();

    controller->registers[IOAPIC_REG_SELECT / 4] = reg;

    asm volatile ("mfence" ::: "memory");

    uint32_t value =
        controller->registers[IOAPIC_REG_WINDOW / 4];

    asm volatile ("mfence" ::: "memory");

    ioapic_restore_interrupts(flags);

    return value;
}

static void ioapic_write_register(
    struct ioapic_controller *controller,
    uint8_t reg,
    uint32_t value
) {
    uint64_t flags = ioapic_save_disable();

    controller->registers[IOAPIC_REG_SELECT / 4] = reg;

    asm volatile ("mfence" ::: "memory");

    controller->registers[IOAPIC_REG_WINDOW / 4] = value;

    asm volatile ("mfence" ::: "memory");

    ioapic_restore_interrupts(flags);
}

static bool ioapic_map_controller(
    struct ioapic_controller *controller,
    uint64_t physical_address,
    uint64_t virtual_address
) {
    if ((physical_address & IOAPIC_PAGE_MASK) != 0) {
        return false;
    }

    if (!ioapic_pat_supports_uc()) {
        return false;
    }

    vmm_mapping_info_t mapping;

    vmm_status_t query =
        vmm_query(virtual_address, &mapping);

    const uint64_t required_cache_flags =
        (1ULL << 3) | (1ULL << 4);

    if (query == VMM_STATUS_OK) {

        if (mapping.physical_address != physical_address ||
            !mapping.writable ||
            mapping.executable ||
            mapping.user_accessible ||
            mapping.huge_page ||
            (mapping.flags & required_cache_flags) !=
                required_cache_flags ||
            (mapping.flags & (1ULL << 7))) {

            return false;
        }

    } else if (query == VMM_STATUS_NOT_MAPPED) {

        if (vmm_map_page(
                virtual_address,
                physical_address,
                VMM_MAP_WRITABLE |
                VMM_MAP_CACHE_DISABLE |
                VMM_MAP_WRITE_THROUGH
            ) != VMM_STATUS_OK) {

            return false;
        }

        controller->mapping_created = true;

        if (vmm_query(virtual_address, &mapping) !=
            VMM_STATUS_OK ||
            mapping.physical_address != physical_address ||
            !mapping.writable ||
            mapping.executable ||
            mapping.user_accessible ||
            mapping.huge_page ||
            (mapping.flags & required_cache_flags) !=
                required_cache_flags ||
            (mapping.flags & (1ULL << 7))) {

            uint64_t ignored;

            vmm_unmap_page(virtual_address, &ignored);

            return false;
        }

    } else {
        return false;
    }

    controller->registers =
        (volatile uint32_t *)(uintptr_t)virtual_address;

    controller->info.physical_address = physical_address;
    controller->info.virtual_address = virtual_address;

    return true;
}

static bool ioapic_mask_all_entries(
    struct ioapic_controller *controller
) {
    for (uint32_t i = 0;
         i < controller->info.redirection_count;
         i++) {

        uint8_t low_reg =
            (uint8_t)(IOAPIC_REG_REDIR_BASE + i * 2U);

        uint32_t low =
            ioapic_read_register(controller, low_reg);

        ioapic_write_register(
            controller,
            low_reg,
            low | IOAPIC_REDIR_MASK
        );

        uint32_t verify =
            ioapic_read_register(controller, low_reg);

        if (!(verify & IOAPIC_REDIR_MASK)) {
            return false;
        }
    }

    return true;
}

static void ioapic_cleanup(void) {

    for (uint32_t i = 0; i < controller_count; i++) {

        if (controllers[i].registers != NULL &&
            controllers[i].mapping_created) {

            uint64_t ignored;

            vmm_unmap_page(
                controllers[i].info.virtual_address,
                &ignored
            );
        }
    }

    memset(
        controllers,
        0,
        sizeof(controllers)
    );

    memset(
        vector_is_ioapic,
        0,
        sizeof(vector_is_ioapic)
    );

    controller_count = 0;
    ioapic_initialized = false;
}

ioapic_status_t ioapic_init(
    const struct acpi_madt_info *madt
) {
    if (ioapic_initialized) {
        return IOAPIC_STATUS_OK;
    }

    if (madt == NULL ||
        madt->io_apic_count == 0 ||
        madt->io_apic_count > IOAPIC_MAX_CONTROLLERS ||
        !lapic_is_initialized()) {

        return IOAPIC_STATUS_INVALID_MADT;
    }

    if (!ioapic_pat_supports_uc()) {
        return IOAPIC_STATUS_PAT_UNSUPPORTED;
    }

    memset(controllers, 0, sizeof(controllers));
    memset(vector_is_ioapic, 0, sizeof(vector_is_ioapic));

    controller_count = madt->io_apic_count;
    saved_madt = *madt;

    for (uint32_t i = 0; i < controller_count; i++) {

        const struct acpi_madt_io_apic *source =
            &madt->io_apics[i];

        struct ioapic_controller *controller =
            &controllers[i];

        if (source->physical_address == 0 ||
            (source->physical_address & IOAPIC_PAGE_MASK)) {

            ioapic_cleanup();
            return IOAPIC_STATUS_INVALID_MADT;
        }

        uint64_t virtual_address =
            IOAPIC_VIRTUAL_BASE + (uint64_t)i * IOAPIC_PAGE_SIZE;

        if (!ioapic_map_controller(
                controller,
                source->physical_address,
                virtual_address)) {

            ioapic_cleanup();
            return IOAPIC_STATUS_MAPPING_FAILED;
        }

        uint32_t version =
            ioapic_read_register(
                controller,
                IOAPIC_REG_VERSION
            );

        uint32_t redirection_count =
            ((version >> 16) & 0xFFU) + 1U;

        if (redirection_count == 0 ||
            redirection_count > IOAPIC_MAX_REDIRECTION_ENTRIES ||
            !ioapic_controller_range_valid(
                source->global_irq_base,
                redirection_count)) {

            ioapic_cleanup();
            return IOAPIC_STATUS_INVALID_MADT;
        }

        controller->info.apic_id =
            (uint8_t)((ioapic_read_register(
                controller,
                IOAPIC_REG_ID
            ) >> 24) & 0x0FU);

        controller->info.version = version & 0xFFU;
        controller->info.gsi_base = source->global_irq_base;
        controller->info.redirection_count = redirection_count;

        /*
         * Verify that this controller's GSI range does not
         * overlap any previously discovered controller.
         */
        uint64_t start = source->global_irq_base;
        uint64_t end = start + redirection_count;

        for (uint32_t j = 0; j < i; j++) {

            uint64_t other_start =
                controllers[j].info.gsi_base;

            uint64_t other_end =
                other_start +
                controllers[j].info.redirection_count;

            if (start < other_end && other_start < end) {
                ioapic_cleanup();
                return IOAPIC_STATUS_INVALID_MADT;
            }
        }

        if (!ioapic_mask_all_entries(controller)) {
            ioapic_cleanup();
            return IOAPIC_STATUS_REGISTER_ACCESS_FAILED;
        }

        controller->initialized = true;
    }

    ioapic_initialized = true;

    return IOAPIC_STATUS_OK;
}

bool ioapic_is_initialized(void) {
    return ioapic_initialized;
}

uint32_t ioapic_get_controller_count(void) {
    return ioapic_initialized ? controller_count : 0;
}

bool ioapic_get_controller_info(
    uint32_t index,
    struct ioapic_controller_info *info
) {
    if (!ioapic_initialized ||
        info == NULL ||
        index >= controller_count) {

        return false;
    }

    *info = controllers[index].info;

    return true;
}

static bool ioapic_read_redirection(
    struct ioapic_controller *controller,
    uint32_t index,
    uint32_t *low,
    uint32_t *high
) {
    if (!controller || !low || !high ||
        index >= controller->info.redirection_count) {

        return false;
    }

    uint8_t low_reg =
        (uint8_t)(IOAPIC_REG_REDIR_BASE + index * 2U);

    *low = ioapic_read_register(controller, low_reg);
    *high = ioapic_read_register(controller, low_reg + 1U);

    return true;
}

bool ioapic_get_route_info(
    uint32_t gsi,
    struct ioapic_route_info *info
) {
    if (!ioapic_initialized || info == NULL) {
        return false;
    }

    uint32_t ci;
    uint32_t ri;

    if (!ioapic_find_controller(gsi, &ci, &ri)) {
        return false;
    }

    uint32_t low;
    uint32_t high;

    if (!ioapic_read_redirection(
            &controllers[ci],
            ri,
            &low,
            &high)) {

        return false;
    }

    info->configured =
    controllers[ci].route_configured[ri];

    info->masked = (low & IOAPIC_REDIR_MASK) != 0;
    info->active_low =
        (low & IOAPIC_REDIR_POLARITY_LOW) != 0;

    info->level_triggered =
        (low & IOAPIC_REDIR_TRIGGER_LEVEL) != 0;

    info->vector = (uint8_t)low;
    info->destination_apic_id =
        (uint8_t)(high >> 24);

    return true;
}

ioapic_status_t ioapic_route_gsi(
    uint32_t gsi,
    uint8_t vector,
    struct ioapic_route_config config,
    uint32_t destination_apic_id
) {
    if (!ioapic_initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (vector < IOAPIC_VECTOR_FIRST ||
        vector > IOAPIC_VECTOR_LAST ||
        vector == LAPIC_TIMER_VECTOR ||
        vector == LAPIC_ERROR_VECTOR ||
        vector == LAPIC_SPURIOUS_VECTOR ||
        config.polarity > IOAPIC_POLARITY_LOW ||
        config.trigger > IOAPIC_TRIGGER_LEVEL) {

        return IOAPIC_STATUS_INVALID_ARGUMENT;
    }

    /*
     * Legacy I/O APIC destination field is eight bits.
     * Do not silently truncate an x2APIC ID.
     */
    if (destination_apic_id > 0xFFU) {
        return IOAPIC_STATUS_UNSUPPORTED;
    }

    uint32_t ci;
    uint32_t ri;

    if (!ioapic_find_controller(gsi, &ci, &ri)) {
        return IOAPIC_STATUS_GSI_NOT_FOUND;
    }

    if (controllers[ci].route_configured[ri]) {
        return IOAPIC_STATUS_GSI_ALREADY_CONFIGURED;
    }

    if (vector_is_ioapic[vector]) {
        /*
         * Sharing a vector between routes requires explicit
         * shared-interrupt ownership support, not implemented
         * in this initial routing API.
         */
        return IOAPIC_STATUS_VECTOR_CONFLICT;
    }

    uint32_t low =
        (uint32_t)vector |
        IOAPIC_REDIR_MASK;

    if (config.polarity == IOAPIC_POLARITY_LOW) {
        low |= IOAPIC_REDIR_POLARITY_LOW;
    }

    if (config.trigger == IOAPIC_TRIGGER_LEVEL) {
        low |= IOAPIC_REDIR_TRIGGER_LEVEL;
    }

    uint32_t high =
        (destination_apic_id & 0xFFU) << 24;

    struct ioapic_controller *controller =
        &controllers[ci];

    uint8_t low_reg =
        (uint8_t)(IOAPIC_REG_REDIR_BASE + ri * 2U);

    /*
     * Keep the entry masked while changing its destination
     * and electrical configuration.
     */
    ioapic_write_register(
        controller,
        low_reg,
        low | IOAPIC_REDIR_MASK
    );

    ioapic_write_register(
        controller,
        low_reg + 1U,
        high
    );

    ioapic_write_register(
        controller,
        low_reg,
        low
    );

    uint32_t verify_low;
    uint32_t verify_high;

    if (!ioapic_read_redirection(
            controller,
            ri,
            &verify_low,
            &verify_high) ||
        (verify_low & 0xFFU) != vector ||
        !(verify_low & IOAPIC_REDIR_MASK) ||
        verify_high != high) {

        ioapic_write_register(
            controller,
            low_reg,
            low | IOAPIC_REDIR_MASK
        );

        return IOAPIC_STATUS_REGISTER_ACCESS_FAILED;
    }

    controllers[ci].route_configured[ri] = true;

    vector_is_ioapic[vector] = true;

    return IOAPIC_STATUS_OK;
}

ioapic_status_t ioapic_route_isa_irq_ex(
    uint8_t isa_irq,
    uint8_t vector,
    uint32_t destination_apic_id,
    uint32_t *resolved_gsi
) {
    if (!ioapic_initialized) {
        return IOAPIC_STATUS_NOT_INITIALIZED;
    }

    if (isa_irq >= 16) {
        return IOAPIC_STATUS_INVALID_ARGUMENT;
    }

    uint32_t gsi = isa_irq;

    struct ioapic_route_config config = {
        .polarity = IOAPIC_POLARITY_HIGH,
        .trigger = IOAPIC_TRIGGER_EDGE
    };

    for (uint32_t i = 0; i < saved_madt.iso_count; i++) {

        const struct acpi_madt_iso *iso =
            &saved_madt.interrupt_overrides[i];

        if (iso->bus != 0 || iso->source != isa_irq) {
            continue;
        }

        gsi = iso->global_irq;

        uint16_t polarity = iso->flags & 3U;
        uint16_t trigger = (iso->flags >> 2) & 3U;

        if (polarity == 2U || trigger == 2U) {
            return IOAPIC_STATUS_INVALID_MADT;
        }

        if (polarity == 3U) {
            config.polarity = IOAPIC_POLARITY_LOW;
        } else {
            config.polarity = IOAPIC_POLARITY_HIGH;
        }

        if (trigger == 3U) {
            config.trigger = IOAPIC_TRIGGER_LEVEL;
        } else {
            config.trigger = IOAPIC_TRIGGER_EDGE;
        }

        break;
    }

    ioapic_status_t status = ioapic_route_gsi(
        gsi,
        vector,
        config,
        destination_apic_id
    );

    if (status == IOAPIC_STATUS_OK &&
        resolved_gsi != NULL) {

        *resolved_gsi = gsi;
    }

    return status;
}

/*
 * Backward-compatible ISA IRQ routing interface.
 */
ioapic_status_t ioapic_route_isa_irq(
    uint8_t isa_irq,
    uint8_t vector,
    uint32_t destination_apic_id
) {
    return ioapic_route_isa_irq_ex(
        isa_irq,
        vector,
        destination_apic_id,
        NULL
    );
}

bool ioapic_mask_gsi(uint32_t gsi) {

    if (!ioapic_initialized) {
        return false;
    }

    uint32_t ci;
    uint32_t ri;

    if (!ioapic_find_controller(gsi, &ci, &ri)) {
        return false;
    }

    struct ioapic_controller *controller =
        &controllers[ci];

    uint8_t low_reg =
        (uint8_t)(IOAPIC_REG_REDIR_BASE + ri * 2U);

    uint32_t low =
        ioapic_read_register(controller, low_reg);

    ioapic_write_register(
        controller,
        low_reg,
        low | IOAPIC_REDIR_MASK
    );

    return (ioapic_read_register(controller, low_reg) &
            IOAPIC_REDIR_MASK) != 0;
}

bool ioapic_unmask_gsi(uint32_t gsi) {

    if (!ioapic_initialized) {
        return false;
    }

    uint32_t ci;
    uint32_t ri;

    if (!ioapic_find_controller(gsi, &ci, &ri)) {
        return false;
    }

    struct ioapic_controller *controller =
        &controllers[ci];

    uint8_t low_reg =
        (uint8_t)(IOAPIC_REG_REDIR_BASE + ri * 2U);

    uint32_t low =
        ioapic_read_register(controller, low_reg);

    uint8_t vector = (uint8_t)low;

    struct irq_stats stats;

    if (!controller->route_configured[ri] ||
        !vector_is_ioapic[vector] ||
        !irq_get_stats(vector, &stats) ||
        !stats.registered) {

        return false;
    }

    ioapic_write_register(
        controller,
        low_reg,
        low & ~IOAPIC_REDIR_MASK
    );

    return (ioapic_read_register(controller, low_reg) &
            IOAPIC_REDIR_MASK) == 0;
}

bool ioapic_eoi_for_vector(uint8_t vector) {

    if (!ioapic_initialized ||
        !vector_is_ioapic[vector]) {

        return false;
    }

    return lapic_eoi();
}
