#ifndef HUA_CLOCKSOURCE_H
#define HUA_CLOCKSOURCE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CLOCKSOURCE_STATUS_OK = 0,
    CLOCKSOURCE_STATUS_NOT_INITIALIZED,
    CLOCKSOURCE_STATUS_ALREADY_INITIALIZED,
    CLOCKSOURCE_STATUS_HARDWARE_UNAVAILABLE,
    CLOCKSOURCE_STATUS_INVALID_ARGUMENT
} clocksource_status_t;

struct clocksource_info {
    bool initialized;
    const char *name;

    uint64_t frequency_hz;
    uint64_t period_femtoseconds;
    uint64_t initial_counter;
};

clocksource_status_t clocksource_init(void);

bool clocksource_is_initialized(void);

clocksource_status_t clocksource_read_ns(uint64_t *nanoseconds);

const struct clocksource_info *clocksource_get_info(void);

#endif
