#ifndef HUA_TIME_H
#define HUA_TIME_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TIME_STATUS_OK = 0,
    TIME_STATUS_NOT_INITIALIZED,
    TIME_STATUS_ALREADY_INITIALIZED,
    TIME_STATUS_CLOCKSOURCE_FAILURE,
    TIME_STATUS_INVALID_ARGUMENT
} time_status_t;

time_status_t time_init(void);

bool time_is_initialized(void);

/*
 * Monotonic time since kernel timekeeping initialization.
 */
time_status_t time_get_monotonic_ns(uint64_t *nanoseconds);

/*
 * Convenience uptime functions.
 */
uint64_t time_get_uptime_us(void);

uint64_t time_get_uptime_ms(void);

uint64_t time_get_uptime_seconds(void);

#endif
