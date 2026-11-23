
#ifndef HUA_HPET_H
#define HUA_HPET_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HPET_STATUS_OK = 0,
    HPET_STATUS_NOT_INITIALIZED,
    HPET_STATUS_ALREADY_INITIALIZED,
    HPET_STATUS_ACPI_NOT_FOUND,
    HPET_STATUS_INVALID_ACPI_TABLE,
    HPET_STATUS_UNSUPPORTED_ADDRESS_SPACE,
    HPET_STATUS_INVALID_ADDRESS,
    HPET_STATUS_PAT_UNSUPPORTED,
    HPET_STATUS_VMM_FAILURE,
    HPET_STATUS_INVALID_MAPPING,
    HPET_STATUS_UNSUPPORTED_COUNTER,
    HPET_STATUS_INVALID_PERIOD,
    HPET_STATUS_INVALID_ARGUMENT
} hpet_status_t;

struct hpet_info {
    bool initialized;
    bool counter_64_bit;
    bool counter_enabled;

    uint64_t physical_address;
    uint64_t period_femtoseconds;
    uint64_t frequency_hz;

    uint16_t minimum_clock_tick;
    uint8_t hpet_number;
    uint16_t vendor_id;

    uint64_t initial_counter;
};

hpet_status_t hpet_init(void);

bool hpet_is_initialized(void);

hpet_status_t hpet_read_counter(uint64_t *counter);

const struct hpet_info *hpet_get_info(void);

uint64_t hpet_get_period_femtoseconds(void);

uint64_t hpet_get_frequency_hz(void);

#endif
