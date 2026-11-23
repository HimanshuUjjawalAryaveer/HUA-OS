
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "tsc.h"

/*
 * TSC runtime state.
 *
 * It is initialized once during early kernel startup,
 * before HUA enables normal interrupt-driven operation.
 */
static struct tsc_info tsc_state;

/*
 * CPUID register output.
 */
static void tsc_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx
) {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;

    asm volatile (
        "cpuid"
        : "=a"(a),
          "=b"(b),
          "=c"(c),
          "=d"(d)
        : "a"(leaf),
          "c"(subleaf)
        : "memory"
    );

    if (eax != NULL) {
        *eax = a;
    }

    if (ebx != NULL) {
        *ebx = b;
    }

    if (ecx != NULL) {
        *ecx = c;
    }

    if (edx != NULL) {
        *edx = d;
    }
}

/*
 * Read the maximum supported basic and extended CPUID leaves.
 */
static void tsc_detect_cpuid_limits(void) {

    uint32_t eax = 0;

    tsc_cpuid(
        0,
        0,
        &eax,
        NULL,
        NULL,
        NULL
    );

    tsc_state.maximum_basic_leaf = eax;

    tsc_cpuid(
        0x80000000U,
        0,
        &eax,
        NULL,
        NULL,
        NULL
    );

    tsc_state.maximum_extended_leaf = eax;
}

/*
 * Detect the architectural TSC feature.
 *
 * CPUID leaf 1, EDX bit 4:
 * TSC instruction support.
 */
static bool tsc_detect_support(void) {

    if (tsc_state.maximum_basic_leaf < 1) {
        return false;
    }

    uint32_t edx = 0;

    tsc_cpuid(
        1,
        0,
        NULL,
        NULL,
        NULL,
        &edx
    );

    return (edx & (1U << 4)) != 0;
}

/*
 * Detect invariant TSC.
 *
 * CPUID 0x80000007, EDX bit 8:
 * Invariant TSC.
 */
static bool tsc_detect_invariant(void) {

    if (tsc_state.maximum_extended_leaf < 0x80000007U) {
        return false;
    }

    uint32_t edx = 0;

    tsc_cpuid(
        0x80000007U,
        0,
        NULL,
        NULL,
        NULL,
        &edx
    );

    return (edx & (1U << 8)) != 0;
}

/*
 * Discover TSC frequency using CPUID leaf 0x15.
 *
 * Architectural formula:
 *
 * TSC frequency =
 *     crystal frequency * numerator / denominator
 *
 * The multiplication is safe because both operands
 * are 32-bit unsigned values. Their product fits in
 * an unsigned 64-bit integer.
 *
 * A zero or unavailable field means that CPUID cannot
 * provide a usable frequency through this method.
 */
static void tsc_detect_frequency(void) {

    if (tsc_state.maximum_basic_leaf < 0x15U) {
        return;
    }

    uint32_t denominator = 0;
    uint32_t numerator = 0;
    uint32_t crystal_hz = 0;

    tsc_cpuid(
        0x15U,
        0,
        &denominator,
        &numerator,
        &crystal_hz,
        NULL
    );

    tsc_state.cpuid_15_denominator = denominator;
    tsc_state.cpuid_15_numerator = numerator;
    tsc_state.cpuid_15_crystal_hz = crystal_hz;

    if (denominator == 0 ||
        numerator == 0 ||
        crystal_hz == 0) {

        return;
    }

    uint64_t product =
        (uint64_t)crystal_hz * (uint64_t)numerator;

    uint64_t frequency =
        product / denominator;

    if (frequency == 0) {
        return;
    }

    tsc_state.frequency_hz = frequency;
    tsc_state.frequency_available = true;
}


/*
 * Initialize TSC detection.
 */
tsc_status_t tsc_init(void) {

    if (tsc_state.initialized) {
        return tsc_state.supported
            ? TSC_STATUS_OK
            : TSC_STATUS_UNSUPPORTED;
    }

    tsc_detect_cpuid_limits();

    tsc_state.supported =
        tsc_detect_support();

    if (!tsc_state.supported) {
        tsc_state.initialized = true;
        return TSC_STATUS_UNSUPPORTED;
    }

    tsc_state.invariant =
        tsc_detect_invariant();

    tsc_detect_frequency();

    tsc_state.initialized = true;

    return TSC_STATUS_OK;
}

/*
 * Check initialization state.
 */
bool tsc_is_initialized(void) {
    return tsc_state.initialized;
}

/*
 * Check TSC support.
 */
bool tsc_is_supported(void) {
    return tsc_state.initialized &&
           tsc_state.supported;
}

/*
 * Check invariant TSC support.
 */
bool tsc_is_invariant(void) {
    return tsc_state.initialized &&
           tsc_state.supported &&
           tsc_state.invariant;
}

/*
 * Read the TSC.
 *
 * LFENCE before RDTSC prevents earlier instructions
 * from being delayed past the timestamp read.
 *
 * LFENCE after RDTSC prevents later instructions from
 * beginning before the timestamp read has completed.
 *
 * The memory clobber also prevents compiler memory
 * operations from being moved across this assembly.
 */
bool tsc_read(uint64_t *value) {

    if (value == NULL ||
        !tsc_is_supported()) {

        return false;
    }

    uint32_t low;
    uint32_t high;

    asm volatile (
        "lfence\n\t"
        "rdtsc\n\t"
        "lfence"
        : "=a"(low),
          "=d"(high)
        :
        : "memory"
    );

    *value =
        ((uint64_t)high << 32) |
        low;

    return true;
}

/*
 * Retrieve CPUID-derived frequency.
 *
 * This is not a calibrated frequency.
 */
bool tsc_get_frequency(uint64_t *frequency_hz) {

    if (frequency_hz == NULL ||
        !tsc_state.initialized ||
        !tsc_state.supported ||
        !tsc_state.frequency_available) {

        return false;
    }

    *frequency_hz = tsc_state.frequency_hz;

    return true;
}

/*
 * Retrieve a copy of TSC information.
 */
bool tsc_get_info(struct tsc_info *info) {

    if (info == NULL ||
        !tsc_state.initialized) {

        return false;
    }

    *info = tsc_state;

    return true;
}
