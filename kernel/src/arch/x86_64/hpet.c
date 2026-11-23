
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "drivers/console/console.h"

#include "hpet.h"
#include "firmware/acpi/acpi.h"
#include "mm/vmm.h"

/*
 * HPET register offsets.
 */
#define HPET_REGISTER_CAPABILITIES  0x000
#define HPET_REGISTER_CONFIGURATION 0x010
#define HPET_REGISTER_INTERRUPT_STATUS 0x020
#define HPET_REGISTER_MAIN_COUNTER  0x0F0

/*
 * HPET register configuration bits.
 */
#define HPET_CONFIGURATION_ENABLE       (1ULL << 0)
#define HPET_CONFIGURATION_LEGACY_ROUTE (1ULL << 1)

/*
 * General capabilities register.
 */
#define HPET_CAPABILITY_COUNTER_64_BIT  (1ULL << 13)

#define HPET_CAPABILITY_VENDOR_SHIFT    16
#define HPET_CAPABILITY_VENDOR_MASK     0xFFFFULL

#define HPET_CAPABILITY_PERIOD_SHIFT    32

/*
 * HPET specification period limits.
 *
 * 1 ns  = 1,000,000 femtoseconds.
 * 100 ns = 100,000,000 femtoseconds.
 */
#define HPET_MIN_PERIOD_FS 1000000ULL
#define HPET_MAX_PERIOD_FS 100000000ULL

/*
 * HPET occupies a separate virtual region.
 *
 * This is distinct from HUA's heap, LAPIC and IOAPIC
 * virtual mapping regions.
 */
#define HPET_VIRTUAL_BASE 0xFFFFFF2000000000ULL

#define HPET_PAGE_SIZE 4096ULL
#define HPET_PAGE_MASK (HPET_PAGE_SIZE - 1ULL)

/*
 * IA32_PAT MSR.
 */
#define IA32_PAT_MSR 0x277U

/*
 * PAT entry 3 must describe uncached memory.
 *
 * Entry 3 occupies bits 24-31.
 */
#define HPET_PAT_ENTRY3_SHIFT 24U
#define HPET_PAT_ENTRY3_MASK  0xFFULL

static struct hpet_info hpet_state;

static volatile uint8_t *hpet_registers;

static bool hpet_mapping_created;


/*
 * Read a model-specific register.
 */
static uint64_t hpet_read_msr(uint32_t msr) {

    uint32_t low;
    uint32_t high;

    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );

    return ((uint64_t)high << 32) | low;
}

static void hpet_debug_write_hex(uint64_t value) {

    const char digits[] = "0123456789ABCDEF";

    console_write("0x");

    for (int i = 60; i >= 0; i -= 4) {

        uint8_t digit = (uint8_t)((value >> i) & 0xF);

        console_put_char(digits[digit]);
    }
}

/*
 * Check CPUID for PAT support.
 */
static bool hpet_pat_supported(void) {

    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile (
        "cpuid"
        : "=a"(eax),
          "=b"(ebx),
          "=c"(ecx),
          "=d"(edx)
        : "a"(1),
          "c"(0)
    );

    return (edx & (1U << 16)) != 0;
}


static bool hpet_pat_supports_uc(void) {
    if (!hpet_pat_supported()) {
        return false;
    }

    uint64_t pat = hpet_read_msr(IA32_PAT_MSR);

    uint8_t entry3 =
        (uint8_t)(
            (pat >> HPET_PAT_ENTRY3_SHIFT) &
            HPET_PAT_ENTRY3_MASK
        );

    return entry3 == 0x00;
}


/*
 * Validate the virtual mapping of the HPET MMIO page.
 */
static bool hpet_mapping_valid(
    uint64_t physical_address
) {

    vmm_mapping_info_t mapping;

    if (vmm_query(
            HPET_VIRTUAL_BASE,
            &mapping
        ) != VMM_STATUS_OK) {

        return false;
    }

    if (mapping.physical_address != physical_address ||
        !mapping.writable ||
        mapping.user_accessible ||
        mapping.executable ||
        mapping.huge_page) {

        return false;
    }

    /*
     * PCD = bit 4.
     * PWT = bit 3.
     * PAT  = bit 7 for a 4 KiB page.
     *
     * PCD + PWT selects PAT entry 3.
     */
    if ((mapping.flags & ((1ULL << 3) | (1ULL << 4))) !=
            ((1ULL << 3) | (1ULL << 4))) {

        return false;
    }

    if ((mapping.flags & (1ULL << 7)) != 0) {
        return false;
    }

    return true;
}


/*
 * Map the HPET hardware register page.
 */
static hpet_status_t hpet_map_mmio(
    uint64_t physical_address
) {

    if (physical_address == 0 ||
        (physical_address & HPET_PAGE_MASK) != 0) {

        return HPET_STATUS_INVALID_ADDRESS;
    }

    console_write("HPET PAT supported: ");
    console_write(hpet_pat_supported() ? "YES\n" : "NO\n");

    if (hpet_pat_supported()) {
        console_write("IA32_PAT: ");
        hpet_debug_write_hex(hpet_read_msr(IA32_PAT_MSR));
        console_write("\n");
    }

    if (!hpet_pat_supports_uc()) {
        return HPET_STATUS_PAT_UNSUPPORTED;
    }

    vmm_mapping_info_t mapping;

    vmm_status_t query_status =
        vmm_query(
            HPET_VIRTUAL_BASE,
            &mapping
        );

    hpet_mapping_created = false;

    if (query_status == VMM_STATUS_NOT_MAPPED) {

        vmm_status_t map_status =
            vmm_map_page(
                HPET_VIRTUAL_BASE,
                physical_address,
                VMM_MAP_WRITABLE |
                VMM_MAP_CACHE_DISABLE |
                VMM_MAP_WRITE_THROUGH
            );

        if (map_status != VMM_STATUS_OK) {
            return HPET_STATUS_VMM_FAILURE;
        }

        hpet_mapping_created = true;

    } else if (query_status != VMM_STATUS_OK) {

        return HPET_STATUS_VMM_FAILURE;
    }

    if (!hpet_mapping_valid(physical_address)) {

        if (hpet_mapping_created) {

            uint64_t unmapped_physical;

            (void)vmm_unmap_page(
                HPET_VIRTUAL_BASE,
                &unmapped_physical
            );

            hpet_mapping_created = false;
        }

        return HPET_STATUS_INVALID_MAPPING;
    }

    hpet_registers =
        (volatile uint8_t *)(uintptr_t)HPET_VIRTUAL_BASE;

    return HPET_STATUS_OK;
}


/*
 * Read a 64-bit HPET register.
 */
static uint64_t hpet_read_register64(
    uint32_t offset
) {

    volatile uint64_t *reg =
        (volatile uint64_t *)(uintptr_t)(
            HPET_VIRTUAL_BASE + offset
        );

    return *reg;
}


/*
 * Write a 64-bit HPET register.
 */
static void hpet_write_register64(
    uint32_t offset,
    uint64_t value
) {

    volatile uint64_t *reg =
        (volatile uint64_t *)(uintptr_t)(
            HPET_VIRTUAL_BASE + offset
        );

    *reg = value;
}


/*
 * Initialize the HPET hardware.
 *
 * The HPET main counter is used as a clocksource.
 * This function does not configure HPET interrupts.
 */
hpet_status_t hpet_init(void) {

    if (hpet_state.initialized) {
        return HPET_STATUS_ALREADY_INITIALIZED;
    }

    hpet_state = (struct hpet_info){0};

    hpet_registers = NULL;
    hpet_mapping_created = false;

    /*
     * Locate the firmware-provided HPET table.
     */
    const struct acpi_sdt_header *header =
        acpi_find_table("HPET");

    if (header == NULL) {
        return HPET_STATUS_ACPI_NOT_FOUND;
    }

    if (header->length != sizeof(struct acpi_hpet_table)) {
        return HPET_STATUS_INVALID_ACPI_TABLE;
    }

    const struct acpi_hpet_table *table =
        (const struct acpi_hpet_table *)header;

    /*
     * HPET must be exposed through system memory.
     */
    if (table->base_address.address_space_id != 0) {
        return HPET_STATUS_UNSUPPORTED_ADDRESS_SPACE;
    }

    if (table->base_address.address == 0 ||
        table->base_address.register_bit_offset != 0) {

        return HPET_STATUS_INVALID_ADDRESS;
    }

    uint64_t physical_address =
        table->base_address.address;

    /*
     * Map the hardware register page.
     */
    hpet_status_t map_status =
        hpet_map_mmio(physical_address);

    if (map_status != HPET_STATUS_OK) {
        return map_status;
    }

    /*
     * Read general capabilities.
     */
    uint64_t capabilities =
        hpet_read_register64(
            HPET_REGISTER_CAPABILITIES
        );

    bool counter_64_bit =
        (capabilities &
         HPET_CAPABILITY_COUNTER_64_BIT) != 0;

    /*
     * This first implementation deliberately requires
     * a 64-bit main counter.
     *
     * A 32-bit counter needs wrap-extension logic before
     * it can safely serve as HUA's long-running clocksource.
     */
    if (!counter_64_bit) {
        return HPET_STATUS_UNSUPPORTED_COUNTER;
    }

    uint64_t period =
        capabilities >> HPET_CAPABILITY_PERIOD_SHIFT;

    if (period < HPET_MIN_PERIOD_FS ||
        period > HPET_MAX_PERIOD_FS) {

        return HPET_STATUS_INVALID_PERIOD;
    }

    /*
     * Extract the hardware vendor ID.
     */
    uint16_t vendor_id =
        (uint16_t)(
            (capabilities >>
             HPET_CAPABILITY_VENDOR_SHIFT) &
            HPET_CAPABILITY_VENDOR_MASK
        );

    /*
     * Read general configuration.
     */
    uint64_t configuration =
        hpet_read_register64(
            HPET_REGISTER_CONFIGURATION
        );

    /*
     * Do not disturb an already enabled counter.
     *
     * If disabled, initialize its counter before enabling.
     */
    if ((configuration & HPET_CONFIGURATION_ENABLE) == 0) {

        configuration &=
            ~HPET_CONFIGURATION_ENABLE;

        hpet_write_register64(
            HPET_REGISTER_CONFIGURATION,
            configuration
        );

        hpet_write_register64(
            HPET_REGISTER_MAIN_COUNTER,
            0
        );

        configuration |=
            HPET_CONFIGURATION_ENABLE;

        hpet_write_register64(
            HPET_REGISTER_CONFIGURATION,
            configuration
        );
    }

    /*
     * Verify that the hardware accepted the enable request.
     */
    configuration =
        hpet_read_register64(
            HPET_REGISTER_CONFIGURATION
        );

    if ((configuration & HPET_CONFIGURATION_ENABLE) == 0) {
        return HPET_STATUS_VMM_FAILURE;
    }

    uint64_t initial_counter =
        hpet_read_register64(
            HPET_REGISTER_MAIN_COUNTER
        );

    /*
     * Convert femtosecond period to integer frequency.
     *
     * frequency = 10^15 / period_fs
     */
    uint64_t frequency =
        1000000000000000ULL / period;

    if (frequency == 0) {
        return HPET_STATUS_INVALID_PERIOD;
    }

    /*
     * Publish initialized state only after validation.
     */
    hpet_state.physical_address = physical_address;
    hpet_state.period_femtoseconds = period;
    hpet_state.frequency_hz = frequency;

    hpet_state.counter_64_bit = true;
    hpet_state.counter_enabled = true;

    hpet_state.minimum_clock_tick =
        table->minimum_clock_tick;

    hpet_state.hpet_number =
        table->hpet_number;

    hpet_state.vendor_id = vendor_id;

    hpet_state.initial_counter = initial_counter;

    hpet_state.initialized = true;

    return HPET_STATUS_OK;
}


/*
 * Check whether HPET initialization succeeded.
 */
bool hpet_is_initialized(void) {

    return hpet_state.initialized;
}


/*
 * Read the current HPET main counter.
 */
hpet_status_t hpet_read_counter(
    uint64_t *counter
) {

    if (counter == NULL) {
        return HPET_STATUS_INVALID_ARGUMENT;
    }

    if (!hpet_state.initialized ||
        hpet_registers == NULL) {

        return HPET_STATUS_NOT_INITIALIZED;
    }

    *counter =
        hpet_read_register64(
            HPET_REGISTER_MAIN_COUNTER
        );

    return HPET_STATUS_OK;
}


/*
 * Retrieve HPET information.
 */
const struct hpet_info *hpet_get_info(void) {

    if (!hpet_state.initialized) {
        return NULL;
    }

    return &hpet_state;
}


/*
 * Return the HPET period in femtoseconds.
 */
uint64_t hpet_get_period_femtoseconds(void) {

    if (!hpet_state.initialized) {
        return 0;
    }

    return hpet_state.period_femtoseconds;
}


/*
 * Return the approximate HPET frequency in Hz.
 */
uint64_t hpet_get_frequency_hz(void) {

    if (!hpet_state.initialized) {
        return 0;
    }

    return hpet_state.frequency_hz;
}
