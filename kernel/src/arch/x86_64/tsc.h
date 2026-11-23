
#ifndef HUA_TSC_H
#define HUA_TSC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TSC_STATUS_OK,
    TSC_STATUS_UNSUPPORTED,
    TSC_STATUS_NOT_INITIALIZED,
    TSC_STATUS_INVALID_ARGUMENT
} tsc_status_t;

struct tsc_info {
    bool initialized;
    bool supported;
    bool invariant;
    bool frequency_available;

    uint32_t maximum_basic_leaf;
    uint32_t maximum_extended_leaf;

    uint32_t cpuid_15_denominator;
    uint32_t cpuid_15_numerator;
    uint32_t cpuid_15_crystal_hz;

    uint64_t frequency_hz;
};

/*
 * Detect TSC capabilities and discover a frequency
 * when CPUID provides sufficient information.
 *
 * This does not calibrate the TSC against another clock.
 */
tsc_status_t tsc_init(void);

/* TSC capability queries. */
bool tsc_is_initialized(void);
bool tsc_is_supported(void);
bool tsc_is_invariant(void);

/*
 * Read the TSC with execution ordering.
 * Returns false if TSC is unavailable or output is NULL.
 */
bool tsc_read(uint64_t *value);

/*
 * Retrieve the CPUID-derived nominal frequency.
 * Returns false if no usable frequency is available.
 */
bool tsc_get_frequency(uint64_t *frequency_hz);

/* Retrieve all detected TSC information. */
bool tsc_get_info(struct tsc_info *info);

#endif
