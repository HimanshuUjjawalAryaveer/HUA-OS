
#ifndef HUA_ACPI_H
#define HUA_ACPI_H

#include <stdbool.h>
#include <stdint.h>

#define ACPI_SDT_SIGNATURE_SIZE 4
#define ACPI_MAX_TABLES 256
#define ACPI_MAX_TABLE_LENGTH (1024ULL * 1024ULL)

struct acpi_info {
    uint8_t revision;
    uint32_t rsdp_length;

    uint32_t rsdt_address;
    uint64_t xsdt_address;

    uint64_t root_table_address;
    bool uses_xsdt;

    uint32_t table_count;
    bool madt_available;
};

/*
 * Standard ACPI System Description Table header.
 * Its packed size must be exactly 36 bytes.
 */
struct __attribute__((packed)) acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};


/*
 * ACPI Generic Address Structure (GAS).
 *
 * Used by ACPI to describe a hardware register address.
 */
struct __attribute__((packed)) acpi_generic_address {
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
};

/*
 * ACPI HPET table.
 *
 * Standard size: 56 bytes.
 */
struct __attribute__((packed)) acpi_hpet_table {
    struct acpi_sdt_header header;

    uint32_t event_timer_block_id;
    struct acpi_generic_address base_address;

    uint8_t hpet_number;
    uint16_t minimum_clock_tick;
    uint8_t page_protection;
};

_Static_assert(
    sizeof(struct acpi_generic_address) == 12,
    "ACPI GAS must be 12 bytes"
);

_Static_assert(
    sizeof(struct acpi_hpet_table) == 56,
    "ACPI HPET table must be 56 bytes"
);



/*
 * MADT record limits.
 *
 * These are fixed-capacity arrays: parsing firmware tables
 * must not depend on dynamic heap allocation.
 */
#define ACPI_MADT_MAX_PROCESSORS 64
#define ACPI_MADT_MAX_IO_APICS 8
#define ACPI_MADT_MAX_ISO 32
#define ACPI_MADT_MAX_NMI 32
#define ACPI_MADT_MAX_NMI_SOURCES 32

/*
 * Fixed MADT header following the standard SDT header.
 */
struct __attribute__((packed)) acpi_madt_header {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
};

/*
 * Discovered processor Local APIC or x2APIC.
 */
struct acpi_madt_processor {
    uint32_t processor_uid;
    uint32_t apic_id;
    uint32_t flags;
    bool x2apic;
};

/*
 * Discovered I/O APIC.
 */
struct acpi_madt_io_apic {
    uint8_t apic_id;
    uint32_t physical_address;
    uint32_t global_irq_base;
};

/*
 * Legacy interrupt source override.
 */
struct acpi_madt_iso {
    uint8_t bus;
    uint8_t source;
    uint32_t global_irq;
    uint16_t flags;
};

/*
 * Local APIC or x2APIC NMI configuration.
 */
struct acpi_madt_nmi {
    uint32_t processor_uid;
    uint16_t flags;
    uint8_t lint;
    bool x2apic;
};


/*
 * Non-Maskable Interrupt Source.
 *
 * MADT type 3, length 8.
 */
struct acpi_madt_nmi_source {
    uint16_t flags;
    uint32_t global_irq;
};


/*
 * Parsed MADT information owned by HUA.
 */
struct acpi_madt_info {
    uint64_t local_apic_address;
    uint32_t flags;

    uint32_t processor_count;
    uint32_t io_apic_count;
    uint32_t iso_count;
    uint32_t nmi_count;
    uint32_t nmi_source_count;

    struct acpi_madt_processor
        processors[ACPI_MADT_MAX_PROCESSORS];

    struct acpi_madt_io_apic
        io_apics[ACPI_MADT_MAX_IO_APICS];

    struct acpi_madt_iso
        interrupt_overrides[ACPI_MADT_MAX_ISO];

    struct acpi_madt_nmi
        nmis[ACPI_MADT_MAX_NMI];

    struct acpi_madt_nmi_source
        nmi_sources[ACPI_MADT_MAX_NMI_SOURCES];

    bool local_apic_address_overridden;
};


/*
 * Initialize ACPI using Limine's RSDP virtual address
 * and the HHDM offset.
 */
bool acpi_init(
    const void *rsdp_address,
    uint64_t hhdm_offset
);

/* Retrieve initialized ACPI information. */
const struct acpi_info *acpi_get_info(void);

/* Retrieve a discovered table by its index. */
const struct acpi_sdt_header *acpi_get_table(uint32_t index);

/* Find a table by its four-character signature. */
const struct acpi_sdt_header *acpi_find_table(
    const char signature[4]
);


/* Parse the discovered MADT. */
bool acpi_madt_parse(void);

/*
 * Parse and validate an independent MADT buffer.
 *
 * This function does not modify the live ACPI/MADT state.
 * It is suitable for isolated tests and validation.
 *
 * buffer_length is the number of accessible bytes.
 * result receives the parsed MADT only on success.
 */
bool acpi_madt_parse_buffer(
    const void *buffer,
    uint32_t buffer_length,
    struct acpi_madt_info *result
);

/* Retrieve parsed MADT information. */
const struct acpi_madt_info *acpi_madt_get_info(void);


#endif
