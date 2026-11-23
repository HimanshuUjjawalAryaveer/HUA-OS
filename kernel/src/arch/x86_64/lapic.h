#ifndef HUA_LAPIC_H
#define HUA_LAPIC_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware/acpi/acpi.h"
#include "arch/x86_64/interrupt_frame.h"

/*
 * Local APIC interrupt vectors.
 *
 * These values are reserved by HUA OS.
 */
#define LAPIC_TIMER_VECTOR       0xE0
#define LAPIC_ERROR_VECTOR       0xFE
#define LAPIC_SPURIOUS_VECTOR    0xFF

/*
 * IA32_APIC_BASE MSR.
 */
#define LAPIC_MSR_APIC_BASE      0x1B

/*
 * IA32_APIC_BASE bits.
 */
#define LAPIC_BASE_BSP           (1ULL << 8)
#define LAPIC_BASE_X2APIC        (1ULL << 10)
#define LAPIC_BASE_GLOBAL_ENABLE (1ULL << 11)

#define LAPIC_BASE_ADDRESS_MASK  0x0000000FFFFFF000ULL

/*
 * IA32_PAT MSR.
 */
#define LAPIC_MSR_PAT            0x277

/*
 * LAPIC register offsets.
 */
#define LAPIC_REG_ID             0x020
#define LAPIC_REG_VERSION        0x030
#define LAPIC_REG_TPR            0x080
#define LAPIC_REG_EOI            0x0B0
#define LAPIC_REG_SVR            0x0F0
#define LAPIC_REG_ESR            0x280

#define LAPIC_REG_LVT_TIMER      0x320
#define LAPIC_REG_LVT_THERMAL    0x330
#define LAPIC_REG_LVT_PERF       0x340
#define LAPIC_REG_LVT_LINT0      0x350
#define LAPIC_REG_LVT_LINT1      0x360
#define LAPIC_REG_LVT_ERROR      0x370

#define LAPIC_REG_TIMER_INITIAL  0x380
#define LAPIC_REG_TIMER_CURRENT  0x390
#define LAPIC_REG_TIMER_DIVIDE   0x3E0

/*
 * Spurious Interrupt Vector Register.
 */
#define LAPIC_SVR_SOFTWARE_ENABLE (1U << 8)

/*
 * LVT bits.
 */
#define LAPIC_LVT_MASKED         (1U << 16)

#define LAPIC_LVT_PERIODIC       (1U << 17)
#define LAPIC_LVT_TSC_DEADLINE   (2U << 17)

/*
 * LVT delivery mode.
 */
#define LAPIC_DELIVERY_NMI       (4U << 8)

/*
 * LVT polarity and trigger.
 */
#define LAPIC_LVT_ACTIVE_LOW     (1U << 13)
#define LAPIC_LVT_LEVEL_TRIGGER  (1U << 15)

/*
 * Timer divider encodings.
 */
typedef enum {
    LAPIC_TIMER_DIVIDE_2   = 0x0,
    LAPIC_TIMER_DIVIDE_4   = 0x1,
    LAPIC_TIMER_DIVIDE_8   = 0x2,
    LAPIC_TIMER_DIVIDE_16  = 0x3,
    LAPIC_TIMER_DIVIDE_32  = 0x8,
    LAPIC_TIMER_DIVIDE_64  = 0x9,
    LAPIC_TIMER_DIVIDE_128 = 0xA,
    LAPIC_TIMER_DIVIDE_1   = 0xB
} lapic_timer_divide_t;

/*
 * Timer operating modes.
 */
typedef enum {
    LAPIC_TIMER_ONE_SHOT,
    LAPIC_TIMER_PERIODIC,
    LAPIC_TIMER_DEADLINE
} lapic_timer_mode_t;

/*
 * Initialization results.
 */
typedef enum {
    LAPIC_STATUS_OK,
    LAPIC_STATUS_NOT_INITIALIZED,
    LAPIC_STATUS_INVALID_ARGUMENT,
    LAPIC_STATUS_UNSUPPORTED_CPU,
    LAPIC_STATUS_INVALID_MADT,
    LAPIC_STATUS_INVALID_APIC_BASE,
    LAPIC_STATUS_UNSUPPORTED_MODE,
    LAPIC_STATUS_PAT_UNSUPPORTED,
    LAPIC_STATUS_MMIO_MAPPING_FAILED,
    LAPIC_STATUS_REGISTER_ACCESS_FAILED,
    LAPIC_STATUS_IRQ_REGISTRATION_FAILED
} lapic_status_t;

/*
 * Runtime information.
 */
struct lapic_info {
    bool initialized;
    bool supported;
    bool x2apic_supported;
    bool x2apic_mode;
    bool globally_enabled;

    uint32_t apic_id;
    uint32_t version;
    uint32_t maximum_lvt_entry;

    uint64_t physical_address;
    uint64_t virtual_address;

    uint64_t timer_interrupt_count;
    uint64_t error_interrupt_count;
    uint64_t spurious_interrupt_count;

    uint32_t last_error_status;
};

/*
 * Initialize the current processor's Local APIC.
 *
 * Requires:
 * - ACPI/MADT initialized.
 * - VMM initialized.
 * - IDT initialized.
 * - Generic IRQ core initialized.
 *
 * Does not enable global CPU interrupts.
 */
lapic_status_t lapic_init(
    const struct acpi_madt_info *madt
);

/*
 * Retrieve LAPIC information.
 */
bool lapic_get_info(struct lapic_info *info);

/*
 * Check initialization state.
 */
bool lapic_is_initialized(void);

/*
 * Read a supported LAPIC register.
 *
 * Register argument is an architectural register offset.
 */
bool lapic_read_register(
    uint32_t offset,
    uint64_t *value
);

/*
 * Send an End Of Interrupt notification.
 */
bool lapic_eoi(void);

/*
 * Handle the LAPIC spurious vector.
 *
 * A spurious interrupt must not receive an EOI.
 */
bool lapic_handle_spurious_interrupt(void);

/*
 * Timer operations.
 */
bool lapic_timer_configure(
    lapic_timer_mode_t mode,
    lapic_timer_divide_t divide,
    uint8_t vector
);

bool lapic_timer_start(uint32_t initial_count);

bool lapic_timer_start_deadline(uint64_t deadline_tsc);

bool lapic_timer_stop(void);

bool lapic_timer_get_current_count(uint32_t *count);

/*
 * Timer frequency is not calibrated by initialization.
 * This reports whether a calibration has been performed.
 */
bool lapic_timer_is_calibrated(void);

/*
 * Publish the LAPIC timer input frequency measured by
 * the generic clockevent calibration layer.
 */
bool lapic_timer_set_calibrated_frequency(uint64_t frequency_hz);

bool lapic_timer_get_calibrated_frequency(uint64_t *frequency_hz);

/*
 * Retrieve the most recent LAPIC error status.
 */
bool lapic_get_error_status(uint32_t *status);

#endif
