#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "time.h"
#include "clocksource.h"

static bool time_initialized;


/*
 * Initialize kernel timekeeping.
 */
time_status_t time_init(void) {

    if (time_initialized) {
        return TIME_STATUS_ALREADY_INITIALIZED;
    }

    clocksource_status_t status =
        clocksource_init();

    if (status != CLOCKSOURCE_STATUS_OK) {
        return TIME_STATUS_CLOCKSOURCE_FAILURE;
    }

    time_initialized = true;

    return TIME_STATUS_OK;
}


/*
 * Check initialization state.
 */
bool time_is_initialized(void) {

    return time_initialized;
}


/*
 * Get monotonic time in nanoseconds.
 */
time_status_t time_get_monotonic_ns(
    uint64_t *nanoseconds
) {

    if (nanoseconds == NULL) {
        return TIME_STATUS_INVALID_ARGUMENT;
    }

    if (!time_initialized) {
        return TIME_STATUS_NOT_INITIALIZED;
    }

    clocksource_status_t status =
        clocksource_read_ns(nanoseconds);

    if (status != CLOCKSOURCE_STATUS_OK) {
        return TIME_STATUS_CLOCKSOURCE_FAILURE;
    }

    return TIME_STATUS_OK;
}


/*
 * Get uptime in microseconds.
 */
uint64_t time_get_uptime_us(void) {

    uint64_t nanoseconds = 0;

    if (time_get_monotonic_ns(&nanoseconds) != TIME_STATUS_OK) {
        return 0;
    }

    return nanoseconds / 1000ULL;
}


/*
 * Get uptime in milliseconds.
 */
uint64_t time_get_uptime_ms(void) {

    uint64_t nanoseconds = 0;

    if (time_get_monotonic_ns(&nanoseconds) != TIME_STATUS_OK) {
        return 0;
    }

    return nanoseconds / 1000000ULL;
}


/*
 * Get uptime in seconds.
 */
uint64_t time_get_uptime_seconds(void) {

    uint64_t nanoseconds = 0;

    if (time_get_monotonic_ns(&nanoseconds) != TIME_STATUS_OK) {
        return 0;
    }

    return nanoseconds / 1000000000ULL;
}
