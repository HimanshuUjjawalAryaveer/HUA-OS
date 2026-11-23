
#include "firmware/acpi/acpi.h"

#include "mm/vmm.h"

#include <stddef.h>
#include <stdint.h>

#define ACPI_RSDP_SIGNATURE "RSD PTR "

#define ACPI_RSDP_V1_SIZE 20
#define ACPI_RSDP_V2_SIZE 36
#define ACPI_RSDP_MAX_SIZE 4096

#define ACPI_PAGE_SIZE 4096ULL
#define ACPI_PAGE_MASK (ACPI_PAGE_SIZE - 1ULL)

struct __attribute__((packed)) acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;

    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

_Static_assert(
    sizeof(struct acpi_sdt_header) == 36,
    "ACPI SDT header must be 36 bytes"
);

static struct acpi_info acpi_state;
static bool acpi_initialized = false;

static uint64_t acpi_hhdm_offset;

static struct acpi_madt_info acpi_madt_state;
static bool acpi_madt_initialized = false;

static const struct acpi_sdt_header *
    acpi_tables[ACPI_MAX_TABLES];


/*
 * Compare two four-character ACPI signatures.
 */
static bool acpi_signature_equal(
    const char *a,
    const char *b
) {
    if (a == NULL || b == NULL) {
        return false;
    }

    for (size_t i = 0; i < ACPI_SDT_SIGNATURE_SIZE; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }

    return true;
}


/*
 * Validate the eight-byte RSDP signature.
 */
static bool acpi_rsdp_signature_valid(const char *signature) {
    if (signature == NULL) {
        return false;
    }

    for (size_t i = 0; i < 8; i++) {
        if (signature[i] != ACPI_RSDP_SIGNATURE[i]) {
            return false;
        }
    }

    return true;
}


/*
 * Calculate an ACPI checksum.
 *
 * A valid ACPI table has an eight-bit sum of zero.
 */
static bool acpi_checksum_valid(
    const void *data,
    uint32_t length
) {
    if (data == NULL || length == 0) {
        return false;
    }

    const uint8_t *bytes = data;
    uint8_t sum = 0;

    for (uint32_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum == 0;
}


/*
 * Check whether an address is canonical for
 * four-level x86-64 paging.
 */
static bool acpi_is_canonical_address(uint64_t address) {
    uint64_t upper = address >> 48;

    bool sign_bit =
        (address & (1ULL << 47)) != 0;

    if (sign_bit) {
        return upper == 0xFFFF;
    }

    return upper == 0;
}


/*
 * Verify that every page in a virtual range is mapped.
 *
 * This is used for Limine's RSDP virtual address.
 */
static bool acpi_virtual_range_mapped(
    uint64_t address,
    uint64_t length
) {
    if (length == 0 ||
        address > UINT64_MAX - (length - 1)) {
        return false;
    }

    uint64_t end = address + length - 1;

    if (!acpi_is_canonical_address(address) ||
        !acpi_is_canonical_address(end)) {
        return false;
    }

    uint64_t page = address & ~ACPI_PAGE_MASK;
    uint64_t last_page = end & ~ACPI_PAGE_MASK;

    while (true) {
        vmm_mapping_info_t mapping;

        if (vmm_query(page, &mapping) != VMM_STATUS_OK) {
            return false;
        }

        if (page == last_page) {
            break;
        }

        if (page > UINT64_MAX - ACPI_PAGE_SIZE) {
            return false;
        }

        page += ACPI_PAGE_SIZE;
    }

    return true;
}


/*
 * Translate a physical ACPI address into its HHDM virtual
 * address and verify that the mapping points to the
 * expected physical memory.
 *
 * ACPI tables may reside in ACPI reclaimable or ACPI NVS
 * memory. These regions must not be rejected merely because
 * they are not ordinary usable RAM.
 */
static const void *acpi_physical_to_virtual(
    uint64_t physical_address,
    uint64_t length
) {
    if (length == 0 ||
        physical_address > UINT64_MAX - (length - 1)) {
        return NULL;
    }

    if (physical_address >
        UINT64_MAX - acpi_hhdm_offset) {
        return NULL;
    }

    uint64_t virtual_address =
        physical_address + acpi_hhdm_offset;

    uint64_t virtual_end =
        virtual_address + length - 1;

    if (!acpi_is_canonical_address(virtual_address) ||
        !acpi_is_canonical_address(virtual_end)) {
        return NULL;
    }

    uint64_t page =
        virtual_address & ~ACPI_PAGE_MASK;

    uint64_t last_page =
        virtual_end & ~ACPI_PAGE_MASK;

    while (true) {
        vmm_mapping_info_t mapping;

        if (vmm_query(page, &mapping) != VMM_STATUS_OK) {
            return NULL;
        }

        if (page < acpi_hhdm_offset ||
            mapping.physical_address !=
                page - acpi_hhdm_offset) {
            return NULL;
        }

        if (page == last_page) {
            break;
        }

        if (page > UINT64_MAX - ACPI_PAGE_SIZE) {
            return NULL;
        }

        page += ACPI_PAGE_SIZE;
    }

    return (const void *)(uintptr_t)virtual_address;
}


/*
 * Retrieve and validate a complete ACPI SDT.
 *
 * First, only the fixed header is accessed.
 * Then its declared length is checked.
 * Finally, the entire table is validated and checksummed.
 */
static const struct acpi_sdt_header *
acpi_validate_table(uint64_t physical_address) {
    if (physical_address == 0) {
        return NULL;
    }

    const struct acpi_sdt_header *header =
        acpi_physical_to_virtual(
            physical_address,
            sizeof(struct acpi_sdt_header)
        );

    if (header == NULL) {
        return NULL;
    }

    uint32_t length = header->length;

    if (length < sizeof(struct acpi_sdt_header) ||
        length > ACPI_MAX_TABLE_LENGTH) {
        return NULL;
    }

    const struct acpi_sdt_header *table =
        acpi_physical_to_virtual(
            physical_address,
            length
        );

    if (table == NULL) {
        return NULL;
    }

    if (!acpi_checksum_valid(table, length)) {
        return NULL;
    }

    return table;
}


/*
 * Initialize the ACPI root table and discover its child tables.
 */
bool acpi_init(
    const void *rsdp_address,
    uint64_t hhdm_offset
) {
    acpi_initialized = false;

    acpi_state = (struct acpi_info){0};

    acpi_hhdm_offset = 0;

    for (uint32_t i = 0; i < ACPI_MAX_TABLES; i++) {
        acpi_tables[i] = NULL;
    }

    if (rsdp_address == NULL || hhdm_offset == 0) {
        return false;
    }

    acpi_hhdm_offset = hhdm_offset;

    uint64_t rsdp_virtual =
        (uint64_t)(uintptr_t)rsdp_address;

    /*
     * Validate the first 20 bytes before reading RSDP fields.
     */
    if (!acpi_virtual_range_mapped(
            rsdp_virtual,
            ACPI_RSDP_V1_SIZE)) {
        return false;
    }

    const struct acpi_rsdp *rsdp = rsdp_address;

    if (!acpi_rsdp_signature_valid(rsdp->signature)) {
        return false;
    }

    if (!acpi_checksum_valid(
            rsdp,
            ACPI_RSDP_V1_SIZE)) {
        return false;
    }

    struct acpi_info info = {0};

    info.revision = rsdp->revision;
    info.rsdt_address = rsdp->rsdt_address;

    /*
     * ACPI 1.0 RSDP.
     */
    if (rsdp->revision == 0) {
        info.rsdp_length = ACPI_RSDP_V1_SIZE;
        info.uses_xsdt = false;
        info.root_table_address = rsdp->rsdt_address;
    }

    /*
     * ACPI 2.0 and later.
     */
    else if (rsdp->revision >= 2) {

        if (!acpi_virtual_range_mapped(
                rsdp_virtual,
                ACPI_RSDP_V2_SIZE)) {
            return false;
        }

        if (rsdp->length < ACPI_RSDP_V2_SIZE ||
            rsdp->length > ACPI_RSDP_MAX_SIZE) {
            return false;
        }

        if (!acpi_virtual_range_mapped(
                rsdp_virtual,
                rsdp->length)) {
            return false;
        }

        if (!acpi_checksum_valid(
                rsdp,
                rsdp->length)) {
            return false;
        }

        info.rsdp_length = rsdp->length;
        info.xsdt_address = rsdp->xsdt_address;

        if (rsdp->xsdt_address != 0) {
            info.uses_xsdt = true;
            info.root_table_address = rsdp->xsdt_address;
        } else {
            info.uses_xsdt = false;
            info.root_table_address = rsdp->rsdt_address;
        }
    }

    /*
     * ACPI revision 1 is unsupported.
     */
    else {
        return false;
    }

    if (info.root_table_address == 0) {
        return false;
    }

    /*
     * Validate the root SDT itself.
     */
    const struct acpi_sdt_header *root =
        acpi_validate_table(info.root_table_address);

    if (root == NULL) {
        return false;
    }

    /*
     * Verify that the root table matches the RSDP.
     */
    if (info.uses_xsdt) {
        if (!acpi_signature_equal(root->signature, "XSDT")) {
            return false;
        }
    } else {
        if (!acpi_signature_equal(root->signature, "RSDT")) {
            return false;
        }
    }

    /*
     * RSDT entries are 32-bit physical addresses.
     * XSDT entries are 64-bit physical addresses.
     */
    uint32_t entry_size =
        info.uses_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);

    uint32_t payload_length =
        root->length - sizeof(struct acpi_sdt_header);

    if (payload_length % entry_size != 0) {
        return false;
    }

    uint32_t entry_count =
        payload_length / entry_size;

    if (entry_count > ACPI_MAX_TABLES) {
        return false;
    }

    const uint8_t *entries =
        (const uint8_t *)root +
        sizeof(struct acpi_sdt_header);

    /*
     * Discover and validate every referenced table.
     *
     * We build a temporary registry first. If an entry is
     * malformed, initialization fails without publishing
     * a partially initialized registry.
     */
    const struct acpi_sdt_header *
        discovered[ACPI_MAX_TABLES] = {0};

    uint32_t discovered_count = 0;
    bool madt_found = false;

    for (uint32_t i = 0; i < entry_count; i++) {

        uint64_t table_physical;

        if (info.uses_xsdt) {
            uint64_t value;

            __builtin_memcpy(
                &value,
                entries + ((uint64_t)i * entry_size),
                sizeof(value)
            );

            table_physical = value;
        } else {
            uint32_t value;

            __builtin_memcpy(
                &value,
                entries + ((uint64_t)i * entry_size),
                sizeof(value)
            );

            table_physical = value;
        }

        if (table_physical == 0) {
            return false;
        }

        const struct acpi_sdt_header *table =
            acpi_validate_table(table_physical);

        if (table == NULL) {
            return false;
        }

        discovered[discovered_count++] = table;

        if (acpi_signature_equal(table->signature, "APIC")) {
            madt_found = true;
        }
    }

    /*
     * Publish the registry only after successful discovery.
     */
    for (uint32_t i = 0; i < discovered_count; i++) {
        acpi_tables[i] = discovered[i];
    }

    info.table_count = discovered_count;
    info.madt_available = madt_found;

    acpi_state = info;
    acpi_initialized = true;

    return true;
}


/*
 * Return ACPI initialization information.
 */
const struct acpi_info *acpi_get_info(void) {
    if (!acpi_initialized) {
        return NULL;
    }

    return &acpi_state;
}


/*
 * Retrieve a discovered ACPI table by index.
 */
const struct acpi_sdt_header *
acpi_get_table(uint32_t index) {
    if (!acpi_initialized ||
        index >= acpi_state.table_count) {
        return NULL;
    }

    return acpi_tables[index];
}


/*
 * Find the first discovered table with the given signature.
 */
const struct acpi_sdt_header *
acpi_find_table(const char signature[4]) {
    if (!acpi_initialized || signature == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < acpi_state.table_count; i++) {

        const struct acpi_sdt_header *table =
            acpi_tables[i];

        if (table != NULL &&
            acpi_signature_equal(
                table->signature,
                signature)) {
            return table;
        }
    }

    return NULL;
}



/*
 * MADT entry types defined by the ACPI specification.
 */
#define ACPI_MADT_TYPE_LOCAL_APIC 0
#define ACPI_MADT_TYPE_IO_APIC 1
#define ACPI_MADT_TYPE_INTERRUPT_OVERRIDE 2
#define ACPI_MADT_TYPE_NMI_SOURCE 3
#define ACPI_MADT_TYPE_LOCAL_APIC_NMI 4
#define ACPI_MADT_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE 5
#define ACPI_MADT_TYPE_LOCAL_X2APIC 9
#define ACPI_MADT_TYPE_LOCAL_X2APIC_NMI 10

#define ACPI_MADT_FIXED_HEADER_SIZE \
    sizeof(struct acpi_madt_header)

#define ACPI_MADT_PAGE_SIZE 4096ULL
#define ACPI_MADT_PAGE_MASK (ACPI_MADT_PAGE_SIZE - 1ULL)

/*
 * Interrupt flags:
 *
 * Bits 0-1: polarity
 *   00 = conforms to bus
 *   01 = active high
 *   10 = reserved
 *   11 = active low
 *
 * Bits 2-3: trigger mode
 *   00 = conforms to bus
 *   01 = edge triggered
 *   10 = reserved
 *   11 = level triggered
 *
 * Bits 4-15 are reserved.
 */
static bool acpi_madt_interrupt_flags_valid(uint16_t flags) {

    if ((flags & 0xFFF0U) != 0) {
        return false;
    }

    uint16_t polarity = flags & 0x3U;
    uint16_t trigger = (flags >> 2) & 0x3U;

    if (polarity == 2U || trigger == 2U) {
        return false;
    }

    return true;
}


/*
 * Check whether a physical MMIO address is nonzero
 * and aligned to a 4 KiB page boundary.
 */
static bool acpi_madt_mmio_address_valid(uint64_t address) {

    return address != 0 &&
           (address & ACPI_MADT_PAGE_MASK) == 0;
}


/*
 * Validate processor flags.
 *
 * Bit 0: enabled
 * Bit 1: online capable
 * All other bits are reserved.
 */
static bool acpi_madt_processor_flags_valid(uint32_t flags) {

    return (flags & ~3U) == 0;
}


/*
 * Check for duplicate processor identifiers.
 *
 * Both processor UID and APIC ID must uniquely identify
 * a processor in the discovered MADT.
 */
static bool acpi_madt_processor_duplicate(
    const struct acpi_madt_info *parsed,
    uint32_t processor_uid,
    uint32_t apic_id
) {

    for (uint32_t i = 0; i < parsed->processor_count; i++) {

        const struct acpi_madt_processor *existing =
            &parsed->processors[i];

        if (existing->processor_uid == processor_uid ||
            existing->apic_id == apic_id) {
            return true;
        }
    }

    return false;
}


/*
 * Check whether an I/O APIC is already described.
 */
static bool acpi_madt_io_apic_duplicate(
    const struct acpi_madt_info *parsed,
    uint8_t apic_id,
    uint32_t physical_address
) {

    for (uint32_t i = 0; i < parsed->io_apic_count; i++) {

        const struct acpi_madt_io_apic *existing =
            &parsed->io_apics[i];

        if (existing->apic_id == apic_id ||
            existing->physical_address == physical_address) {
            return true;
        }
    }

    return false;
}


/*
 * Check whether an interrupt source override is duplicated.
 */
static bool acpi_madt_iso_duplicate(
    const struct acpi_madt_info *parsed,
    uint8_t bus,
    uint8_t source
) {

    for (uint32_t i = 0; i < parsed->iso_count; i++) {

        const struct acpi_madt_iso *existing =
            &parsed->interrupt_overrides[i];

        if (existing->bus == bus &&
            existing->source == source) {
            return true;
        }
    }

    return false;
}


/*
 * Check whether an NMI source is duplicated.
 */
static bool acpi_madt_nmi_source_duplicate(
    const struct acpi_madt_info *parsed,
    uint32_t global_irq
) {

    for (uint32_t i = 0; i < parsed->nmi_source_count; i++) {

        if (parsed->nmi_sources[i].global_irq == global_irq) {
            return true;
        }
    }

    return false;
}


/*
 * Parse and validate a standalone MADT buffer.
 *
 * This function:
 *
 * - Does not depend on ACPI initialization.
 * - Does not allocate memory.
 * - Does not access hardware.
 * - Does not modify global ACPI state.
 * - Does not publish partial results.
 *
 * The caller must provide a readable buffer of buffer_length
 * bytes and a writable result structure.
 */
bool acpi_madt_parse_buffer(
    const void *buffer,
    uint32_t buffer_length,
    struct acpi_madt_info *result
) {

    if (buffer == NULL || result == NULL) {
        return false;
    }

    if (buffer_length < ACPI_MADT_FIXED_HEADER_SIZE) {
        return false;
    }

    const uint8_t *bytes = buffer;

    /*
     * Validate the standard ACPI table signature.
     */
    if (!acpi_signature_equal(
            (const char *)bytes,
            "APIC")) {
        return false;
    }

    /*
     * Read the declared table length without assuming
     * the caller's buffer is naturally aligned.
     */
    uint32_t table_length = 0;

    __builtin_memcpy(
        &table_length,
        &bytes[4],
        sizeof(table_length)
    );

    if (table_length < ACPI_MADT_FIXED_HEADER_SIZE ||
        table_length > buffer_length ||
        table_length > ACPI_MAX_TABLE_LENGTH) {
        return false;
    }

    /*
     * Validate the complete SDT checksum.
     */
    if (!acpi_checksum_valid(buffer, table_length)) {
        return false;
    }

    struct acpi_madt_info parsed = {0};

    /*
     * MADT fixed fields:
     *
     * Offset 36: Local APIC physical address (32-bit)
     * Offset 40: MADT flags (32-bit)
     */
    uint32_t local_apic_address = 0;

    __builtin_memcpy(
        &local_apic_address,
        &bytes[36],
        sizeof(local_apic_address)
    );

    __builtin_memcpy(
        &parsed.flags,
        &bytes[40],
        sizeof(parsed.flags)
    );

    if (!acpi_madt_mmio_address_valid(local_apic_address)) {
        return false;
    }

    /*
     * Only bit 0 is currently defined in the MADT flags.
     * Bit 0 indicates legacy dual 8259 PIC compatibility.
     */
    if ((parsed.flags & ~1U) != 0) {
        return false;
    }

    parsed.local_apic_address = local_apic_address;

    uint32_t offset = ACPI_MADT_FIXED_HEADER_SIZE;

    while (offset < table_length) {

        uint32_t remaining = table_length - offset;

        /*
         * Every record requires at least:
         *
         * Byte 0: Type
         * Byte 1: Length
         */
        if (remaining < 2) {
            return false;
        }

        const uint8_t *entry = &bytes[offset];

        uint8_t type = entry[0];
        uint8_t length = entry[1];

        if (length < 2 || length > remaining) {
            return false;
        }

        switch (type) {

            /*
             * Type 0: Processor Local APIC.
             * Length: 8 bytes.
             */
            case ACPI_MADT_TYPE_LOCAL_APIC: {

                if (length != 8 ||
                    parsed.processor_count >=
                        ACPI_MADT_MAX_PROCESSORS) {
                    return false;
                }

                struct acpi_madt_processor processor = {0};

                processor.processor_uid = entry[2];
                processor.apic_id = entry[3];

                __builtin_memcpy(
                    &processor.flags,
                    &entry[4],
                    sizeof(processor.flags)
                );

                processor.x2apic = false;

                if (!acpi_madt_processor_flags_valid(
                        processor.flags)) {
                    return false;
                }

                if (acpi_madt_processor_duplicate(
                        &parsed,
                        processor.processor_uid,
                        processor.apic_id)) {
                    return false;
                }

                parsed.processors[parsed.processor_count++] =
                    processor;

                break;
            }

            /*
             * Type 1: I/O APIC.
             * Length: 12 bytes.
             */
            case ACPI_MADT_TYPE_IO_APIC: {

                if (length != 12 ||
                    parsed.io_apic_count >=
                        ACPI_MADT_MAX_IO_APICS) {
                    return false;
                }

                struct acpi_madt_io_apic io_apic = {0};

                io_apic.apic_id = entry[2];

                __builtin_memcpy(
                    &io_apic.physical_address,
                    &entry[4],
                    sizeof(io_apic.physical_address)
                );

                __builtin_memcpy(
                    &io_apic.global_irq_base,
                    &entry[8],
                    sizeof(io_apic.global_irq_base)
                );

                if (!acpi_madt_mmio_address_valid(
                        io_apic.physical_address)) {
                    return false;
                }

                if (acpi_madt_io_apic_duplicate(
                        &parsed,
                        io_apic.apic_id,
                        io_apic.physical_address)) {
                    return false;
                }

                parsed.io_apics[parsed.io_apic_count++] =
                    io_apic;

                break;
            }

            /*
             * Type 2: Interrupt Source Override.
             * Length: 10 bytes.
             */
            case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE: {

                if (length != 10 ||
                    parsed.iso_count >= ACPI_MADT_MAX_ISO) {
                    return false;
                }

                struct acpi_madt_iso iso = {0};

                iso.bus = entry[2];
                iso.source = entry[3];

                __builtin_memcpy(
                    &iso.global_irq,
                    &entry[4],
                    sizeof(iso.global_irq)
                );

                __builtin_memcpy(
                    &iso.flags,
                    &entry[8],
                    sizeof(iso.flags)
                );

                if (!acpi_madt_interrupt_flags_valid(iso.flags)) {
                    return false;
                }

                if (acpi_madt_iso_duplicate(
                        &parsed,
                        iso.bus,
                        iso.source)) {
                    return false;
                }

                parsed.interrupt_overrides[parsed.iso_count++] =
                    iso;

                break;
            }

            /*
             * Type 3: NMI Source.
             * Length: 8 bytes.
             */
            case ACPI_MADT_TYPE_NMI_SOURCE: {

                if (length != 8 ||
                    parsed.nmi_source_count >=
                        ACPI_MADT_MAX_NMI_SOURCES) {
                    return false;
                }

                struct acpi_madt_nmi_source nmi_source = {0};

                __builtin_memcpy(
                    &nmi_source.flags,
                    &entry[2],
                    sizeof(nmi_source.flags)
                );

                __builtin_memcpy(
                    &nmi_source.global_irq,
                    &entry[4],
                    sizeof(nmi_source.global_irq)
                );

                if (!acpi_madt_interrupt_flags_valid(
                        nmi_source.flags)) {
                    return false;
                }

                if (acpi_madt_nmi_source_duplicate(
                        &parsed,
                        nmi_source.global_irq)) {
                    return false;
                }

                parsed.nmi_sources[parsed.nmi_source_count++] =
                    nmi_source;

                break;
            }

            /*
             * Type 4: Local APIC NMI.
             * Length: 6 bytes.
             */
            case ACPI_MADT_TYPE_LOCAL_APIC_NMI: {

                if (length != 6 ||
                    parsed.nmi_count >= ACPI_MADT_MAX_NMI) {
                    return false;
                }

                struct acpi_madt_nmi nmi = {0};

                nmi.processor_uid = entry[2];

                __builtin_memcpy(
                    &nmi.flags,
                    &entry[3],
                    sizeof(nmi.flags)
                );

                nmi.lint = entry[5];
                nmi.x2apic = false;

                if (!acpi_madt_interrupt_flags_valid(nmi.flags) ||
                    nmi.lint > 1) {
                    return false;
                }

                parsed.nmis[parsed.nmi_count++] = nmi;

                break;
            }

            /*
             * Type 5: Local APIC Address Override.
             * Length: 12 bytes.
             */
            case ACPI_MADT_TYPE_LOCAL_APIC_ADDRESS_OVERRIDE: {

                if (length != 12 ||
                    parsed.local_apic_address_overridden) {
                    return false;
                }

                uint64_t address = 0;

                __builtin_memcpy(
                    &address,
                    &entry[4],
                    sizeof(address)
                );

                if (!acpi_madt_mmio_address_valid(address)) {
                    return false;
                }

                parsed.local_apic_address = address;
                parsed.local_apic_address_overridden = true;

                break;
            }

            /*
             * Type 9: Processor Local x2APIC.
             * Length: 16 bytes.
             */
            case ACPI_MADT_TYPE_LOCAL_X2APIC: {

                if (length != 16 ||
                    parsed.processor_count >=
                        ACPI_MADT_MAX_PROCESSORS) {
                    return false;
                }

                struct acpi_madt_processor processor = {0};

                __builtin_memcpy(
                    &processor.apic_id,
                    &entry[4],
                    sizeof(processor.apic_id)
                );

                __builtin_memcpy(
                    &processor.flags,
                    &entry[8],
                    sizeof(processor.flags)
                );

                __builtin_memcpy(
                    &processor.processor_uid,
                    &entry[12],
                    sizeof(processor.processor_uid)
                );

                processor.x2apic = true;

                if (!acpi_madt_processor_flags_valid(
                        processor.flags)) {
                    return false;
                }

                if (acpi_madt_processor_duplicate(
                        &parsed,
                        processor.processor_uid,
                        processor.apic_id)) {
                    return false;
                }

                parsed.processors[parsed.processor_count++] =
                    processor;

                break;
            }

            /*
             * Type 10: Local x2APIC NMI.
             * Length: 12 bytes.
             */
            case ACPI_MADT_TYPE_LOCAL_X2APIC_NMI: {

                if (length != 12 ||
                    parsed.nmi_count >= ACPI_MADT_MAX_NMI) {
                    return false;
                }

                struct acpi_madt_nmi nmi = {0};

                __builtin_memcpy(
                    &nmi.flags,
                    &entry[2],
                    sizeof(nmi.flags)
                );

                __builtin_memcpy(
                    &nmi.processor_uid,
                    &entry[4],
                    sizeof(nmi.processor_uid)
                );

                nmi.lint = entry[8];
                nmi.x2apic = true;

                if (!acpi_madt_interrupt_flags_valid(nmi.flags) ||
                    nmi.lint > 1) {
                    return false;
                }

                parsed.nmis[parsed.nmi_count++] = nmi;

                break;
            }

            /*
             * Unknown record types.
             *
             * The common record header and declared length
             * have already been validated. Skip safely.
             */
            default:
                break;
        }

        offset += length;
    }

    /*
     * Publish only after the entire table is valid.
     */
    *result = parsed;

    return true;
}


/*
 * Parse the MADT discovered by the live ACPI subsystem.
 *
 * The independent parser performs all validation.
 * Live state is changed only after complete success.
 */
bool acpi_madt_parse(void) {

    acpi_madt_initialized = false;
    acpi_madt_state = (struct acpi_madt_info){0};

    if (!acpi_initialized) {
        return false;
    }

    const struct acpi_sdt_header *table =
        acpi_find_table("APIC");

    if (table == NULL) {
        return false;
    }

    struct acpi_madt_info parsed = {0};

    if (!acpi_madt_parse_buffer(
            table,
            table->length,
            &parsed)) {
        return false;
    }

    acpi_madt_state = parsed;
    acpi_madt_initialized = true;

    return true;
}


/*
 * Return parsed MADT information.
 */
const struct acpi_madt_info *acpi_madt_get_info(void) {

    if (!acpi_madt_initialized) {
        return NULL;
    }

    return &acpi_madt_state;
}
