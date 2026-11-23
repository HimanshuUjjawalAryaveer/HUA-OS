
#ifndef HUA_IOAPIC_H
#define HUA_IOAPIC_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware/acpi/acpi.h"

#define IOAPIC_MAX_CONTROLLERS ACPI_MADT_MAX_IO_APICS

#define IOAPIC_VECTOR_FIRST 0x20
#define IOAPIC_VECTOR_LAST  0xFD

typedef enum {
    IOAPIC_STATUS_OK,
    IOAPIC_STATUS_NOT_INITIALIZED,
    IOAPIC_STATUS_INVALID_ARGUMENT,
    IOAPIC_STATUS_UNSUPPORTED,
    IOAPIC_STATUS_PAT_UNSUPPORTED,
    IOAPIC_STATUS_MAPPING_FAILED,
    IOAPIC_STATUS_REGISTER_ACCESS_FAILED,
    IOAPIC_STATUS_INVALID_MADT,
    IOAPIC_STATUS_GSI_NOT_FOUND,
    IOAPIC_STATUS_VECTOR_CONFLICT,
    IOAPIC_STATUS_GSI_ALREADY_CONFIGURED,
    IOAPIC_STATUS_OUT_OF_RANGE
} ioapic_status_t;

/*
 * Electrical configuration.
 *
 * Always specify both polarity and trigger mode.
 */
typedef enum {
    IOAPIC_POLARITY_HIGH,
    IOAPIC_POLARITY_LOW
} ioapic_polarity_t;

typedef enum {
    IOAPIC_TRIGGER_EDGE,
    IOAPIC_TRIGGER_LEVEL
} ioapic_trigger_t;

struct ioapic_route_config {
    ioapic_polarity_t polarity;
    ioapic_trigger_t trigger;
};

struct ioapic_controller_info {
    uint8_t apic_id;
    uint32_t version;
    uint32_t gsi_base;
    uint32_t redirection_count;
    uint64_t physical_address;
    uint64_t virtual_address;
};

struct ioapic_route_info {
    bool configured;
    bool masked;
    bool active_low;
    bool level_triggered;
    uint8_t vector;
    uint8_t destination_apic_id;
};

ioapic_status_t ioapic_init(
    const struct acpi_madt_info *madt
);

bool ioapic_is_initialized(void);

uint32_t ioapic_get_controller_count(void);

bool ioapic_get_controller_info(
    uint32_t index,
    struct ioapic_controller_info *info
);

bool ioapic_get_route_info(
    uint32_t gsi,
    struct ioapic_route_info *info
);

/*
 * Configure a route but always leave it masked.
 */
ioapic_status_t ioapic_route_gsi(
    uint32_t gsi,
    uint8_t vector,
    struct ioapic_route_config config,
    uint32_t destination_apic_id
);

/*
 * Resolve a legacy ISA IRQ using MADT interrupt-source
 * overrides, then configure the resulting GSI masked.
 */
ioapic_status_t ioapic_route_isa_irq(
    uint8_t isa_irq,
    uint8_t vector,
    uint32_t destination_apic_id
);

/*
 * Extended ISA IRQ routing.
 *
 * Resolves MADT overrides and configures the route masked.
 * If resolved_gsi is non-NULL, returns the actual GSI on success.
 */
ioapic_status_t ioapic_route_isa_irq_ex(
    uint8_t isa_irq,
    uint8_t vector,
    uint32_t destination_apic_id,
    uint32_t *resolved_gsi
);

bool ioapic_mask_gsi(uint32_t gsi);

/*
 * Unmask only when the vector already has a registered
 * generic IRQ handler.
 */
bool ioapic_unmask_gsi(uint32_t gsi);

/*
 * Called by the IDT after successful generic IRQ dispatch.
 *
 * Sends LAPIC EOI only when this vector belongs to an
 * I/O APIC route.
 */
bool ioapic_eoi_for_vector(uint8_t vector);

#endif
