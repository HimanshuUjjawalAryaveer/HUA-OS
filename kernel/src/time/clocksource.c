#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>

#include "clocksource.h"
#include "arch/x86_64/hpet.h"

static struct clocksource_info clocksource_state;

static uint64_t clocksource_initial_counter;


/*
 * Convert HPET ticks into nanoseconds.
 *
 * HPET period is expressed in femtoseconds.
 *
 * 1 nanosecond = 1,000,000 femtoseconds.
 *
 * Splitting the calculation avoids multiplying the
 * entire counter delta by the period at once.
 */
static uint64_t clocksource_ticks_to_ns(uint64_t ticks) {

    uint64_t period =
        clocksource_state.period_femtoseconds;

    if (period == 0) {
        return 0;
    }

    uint64_t whole =
        ticks / 1000000ULL;

    uint64_t remainder =
        ticks % 1000000ULL;

    /*
     * Check multiplication overflow.
     */
    if (whole > UINT64_MAX / period) {
        return UINT64_MAX;
    }

    uint64_t result =
        whole * period;

    uint64_t fractional =
        (remainder * period) / 1000000ULL;

    /*
     * Check addition overflow.
     */
    if (result > UINT64_MAX - fractional) {
        return UINT64_MAX;
    }

    return result + fractional;
}


/*
 * Initialize the generic clocksource using HPET.
 */
clocksource_status_t clocksource_init(void) {

    if (clocksource_state.initialized) {
        return CLOCKSOURCE_STATUS_ALREADY_INITIALIZED;
    }

    if (!hpet_is_initialized()) {
        return CLOCKSOURCE_STATUS_HARDWARE_UNAVAILABLE;
    }

    const struct hpet_info *hpet =
        hpet_get_info();

    if (hpet == NULL ||
        !hpet->counter_enabled ||
        hpet->period_femtoseconds == 0 ||
        hpet->frequency_hz == 0) {

        return CLOCKSOURCE_STATUS_HARDWARE_UNAVAILABLE;
    }

    uint64_t initial_counter = 0;

    if (hpet_read_counter(&initial_counter) != HPET_STATUS_OK) {
        return CLOCKSOURCE_STATUS_HARDWARE_UNAVAILABLE;
    }

    clocksource_state.name = "HPET";

    clocksource_state.frequency_hz =
        hpet->frequency_hz;

    clocksource_state.period_femtoseconds =
        hpet->period_femtoseconds;

    clocksource_state.initial_counter =
        initial_counter;

    clocksource_initial_counter =
        initial_counter;

    clocksource_state.initialized = true;

    return CLOCKSOURCE_STATUS_OK;
}


/*
 * Check initialization state.
 */
bool clocksource_is_initialized(void) {

    return clocksource_state.initialized;
}


/*
 * Return elapsed nanoseconds since clocksource initialization.
 */
clocksource_status_t clocksource_read_ns(
    uint64_t *nanoseconds
) {

    if (nanoseconds == NULL) {
        return CLOCKSOURCE_STATUS_INVALID_ARGUMENT;
    }

    if (!clocksource_state.initialized) {
        return CLOCKSOURCE_STATUS_NOT_INITIALIZED;
    }

    uint64_t current_counter = 0;

    if (hpet_read_counter(&current_counter) != HPET_STATUS_OK) {
        return CLOCKSOURCE_STATUS_HARDWARE_UNAVAILABLE;
    }

    /*
     * Unsigned subtraction also handles one 64-bit
     * counter wraparound.
     */
    uint64_t elapsed_ticks =
        current_counter - clocksource_initial_counter;

    *nanoseconds =
        clocksource_ticks_to_ns(elapsed_ticks);

    return CLOCKSOURCE_STATUS_OK;
}


/*
 * Retrieve clocksource information.
 */
const struct clocksource_info *clocksource_get_info(void) {

    if (!clocksource_state.initialized) {
        return NULL;
    }

    return &clocksource_state;
}
