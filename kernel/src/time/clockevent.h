#ifndef HUA_CLOCKEVENT_H
#define HUA_CLOCKEVENT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CLOCKEVENT_STATUS_OK = 0,
    CLOCKEVENT_STATUS_NOT_INITIALIZED,
    CLOCKEVENT_STATUS_ALREADY_INITIALIZED,
    CLOCKEVENT_STATUS_ALREADY_STARTED,
    CLOCKEVENT_STATUS_HPET_UNAVAILABLE,
    CLOCKEVENT_STATUS_LAPIC_UNAVAILABLE,
    CLOCKEVENT_STATUS_CALIBRATION_FAILED,
    CLOCKEVENT_STATUS_INVALID_ARGUMENT,
    CLOCKEVENT_STATUS_TIMER_FAILURE
} clockevent_status_t;

struct clockevent_info {
    bool initialized;
    bool calibrated;
    bool started;

    uint64_t lapic_frequency_hz;

    uint32_t periodic_frequency_hz;
    uint32_t periodic_initial_count;

    uint32_t divide_factor;
};

clockevent_status_t clockevent_init(void);

bool clockevent_is_initialized(void);

clockevent_status_t clockevent_start_periodic(
    uint32_t frequency_hz
);

clockevent_status_t clockevent_stop(void);

bool clockevent_get_info(struct clockevent_info *info);

bool clockevent_get_interrupt_count(uint64_t *count);

#endif
