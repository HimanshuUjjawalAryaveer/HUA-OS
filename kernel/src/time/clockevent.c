#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>

#include "clockevent.h"

#include "arch/x86_64/hpet.h"
#include "arch/x86_64/lapic.h"

#define CLOCKEVENT_CALIBRATION_MS       20ULL
#define CLOCKEVENT_CALIBRATION_DIVISOR  50ULL
#define CLOCKEVENT_CALIBRATION_COUNT    UINT32_MAX

#define CLOCKEVENT_MAX_POLL_ITERATIONS  100000000ULL

struct clockevent_state {
    bool initialized;
    bool started;

    uint64_t lapic_frequency_hz;

    uint32_t periodic_frequency_hz;
    uint32_t periodic_initial_count;
    uint32_t divide_factor;
};

static struct clockevent_state clockevent_state;


/*
 * Check whether multiplication can be performed safely.
 */
static bool clockevent_multiply_u64(
    uint64_t a,
    uint64_t b,
    uint64_t *result
) {

    if (result == NULL) {
        return false;
    }

    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }

    *result = a * b;

    return true;
}


/*
 * Calibration must run with CPU interrupts disabled.
 *
 * This prevents unrelated interrupt handlers from
 * disturbing the polling measurement.
 */
static bool clockevent_interrupts_disabled(void) {

    uint64_t rflags;

    asm volatile (
        "pushfq\n"
        "popq %0"
        : "=r"(rflags)
        :
        : "memory"
    );

    return (rflags & (1ULL << 9)) == 0;
}


/*
 * Measure the LAPIC timer input frequency against HPET.
 *
 * The LAPIC timer runs in one-shot mode with divide-by-16.
 * Its initial count is UINT32_MAX.
 *
 * We measure the number of LAPIC timer counts consumed
 * during a 20 ms HPET interval.
 */
static bool clockevent_calibrate_lapic(void) {

    if (!hpet_is_initialized() ||
        !lapic_is_initialized() ||
        !clockevent_interrupts_disabled()) {

        return false;
    }

    uint64_t hpet_frequency =
        hpet_get_frequency_hz();

    if (hpet_frequency < CLOCKEVENT_CALIBRATION_DIVISOR) {
        return false;
    }

    uint64_t target_hpet_ticks =
        hpet_frequency / CLOCKEVENT_CALIBRATION_DIVISOR;

    if (target_hpet_ticks == 0) {
        return false;
    }

    /*
     * Ensure the timer begins from a known stopped state.
     */
    if (!lapic_timer_stop()) {
        return false;
    }

    if (!lapic_timer_configure(
            LAPIC_TIMER_ONE_SHOT,
            LAPIC_TIMER_DIVIDE_16,
            LAPIC_TIMER_VECTOR)) {

        return false;
    }

    uint64_t hpet_start = 0;

    if (hpet_read_counter(&hpet_start) != HPET_STATUS_OK) {
        lapic_timer_stop();
        return false;
    }

    /*
     * Start the LAPIC timer.
     *
     * Its interrupt remains harmless during this short
     * calibration interval because CPU interrupts are disabled.
     */
    if (!lapic_timer_start(CLOCKEVENT_CALIBRATION_COUNT)) {
        lapic_timer_stop();
        return false;
    }

    uint64_t hpet_end = hpet_start;

    bool reference_elapsed = false;

    for (uint64_t i = 0;
         i < CLOCKEVENT_MAX_POLL_ITERATIONS;
         i++) {

        if (hpet_read_counter(&hpet_end) != HPET_STATUS_OK) {
            lapic_timer_stop();
            return false;
        }

        uint64_t elapsed =
            hpet_end - hpet_start;

        if (elapsed >= target_hpet_ticks) {
            reference_elapsed = true;
            break;
        }
    }

    if (!reference_elapsed) {
        lapic_timer_stop();
        return false;
    }

    /*
     * Read the LAPIC current count before stopping it.
     */
    uint32_t current_count = 0;

    bool count_read =
        lapic_timer_get_current_count(&current_count);

    bool stopped = lapic_timer_stop();

    if (!count_read || !stopped) {
        return false;
    }

    if (current_count >= CLOCKEVENT_CALIBRATION_COUNT) {
        return false;
    }

    uint64_t elapsed_lapic_counts =
        (uint64_t)CLOCKEVENT_CALIBRATION_COUNT -
        current_count;

    uint64_t elapsed_hpet_ticks =
        hpet_end - hpet_start;

    if (elapsed_lapic_counts == 0 ||
        elapsed_hpet_ticks == 0) {

        return false;
    }

    /*
     * Formula:
     *
     * LAPIC input frequency =
     *
     * elapsed LAPIC counts
     * * divider
     * * HPET frequency
     * / elapsed HPET ticks
     *
     * Divide-by-16 was used during measurement.
     */
    uint64_t numerator = 0;

    if (!clockevent_multiply_u64(
            elapsed_lapic_counts,
            16ULL,
            &numerator)) {

        return false;
    }

    if (!clockevent_multiply_u64(
            numerator,
            hpet_frequency,
            &numerator)) {

        return false;
    }

    uint64_t measured_frequency =
        numerator / elapsed_hpet_ticks;

    if (measured_frequency == 0) {
        return false;
    }

    /*
     * Publish the measured frequency to the LAPIC driver.
     */
    if (!lapic_timer_set_calibrated_frequency(
            measured_frequency)) {

        return false;
    }

    clockevent_state.lapic_frequency_hz =
        measured_frequency;

    return true;
}


/*
 * Initialize and calibrate the LAPIC clockevent.
 */
clockevent_status_t clockevent_init(void) {

    if (clockevent_state.initialized) {
        return CLOCKEVENT_STATUS_ALREADY_INITIALIZED;
    }

    if (!hpet_is_initialized()) {
        return CLOCKEVENT_STATUS_HPET_UNAVAILABLE;
    }

    if (!lapic_is_initialized()) {
        return CLOCKEVENT_STATUS_LAPIC_UNAVAILABLE;
    }

    if (!clockevent_interrupts_disabled()) {
        return CLOCKEVENT_STATUS_CALIBRATION_FAILED;
    }

    if (!clockevent_calibrate_lapic()) {
        return CLOCKEVENT_STATUS_CALIBRATION_FAILED;
    }

    clockevent_state.initialized = true;

    return CLOCKEVENT_STATUS_OK;
}


bool clockevent_is_initialized(void) {

    return clockevent_state.initialized;
}


/*
 * Select a divider and calculate the initial count
 * for the requested periodic interrupt frequency.
 */
static bool clockevent_calculate_periodic_count(
    uint32_t frequency_hz,
    uint32_t *initial_count,
    uint32_t *divide_factor,
    lapic_timer_divide_t *divide_encoding
) {

    if (frequency_hz == 0 ||
        initial_count == NULL ||
        divide_factor == NULL ||
        divide_encoding == NULL) {

        return false;
    }

    static const struct {
        uint32_t factor;
        lapic_timer_divide_t encoding;
    } choices[] = {
        { 1,   LAPIC_TIMER_DIVIDE_1 },
        { 2,   LAPIC_TIMER_DIVIDE_2 },
        { 4,   LAPIC_TIMER_DIVIDE_4 },
        { 8,   LAPIC_TIMER_DIVIDE_8 },
        { 16,  LAPIC_TIMER_DIVIDE_16 },
        { 32,  LAPIC_TIMER_DIVIDE_32 },
        { 64,  LAPIC_TIMER_DIVIDE_64 },
        { 128, LAPIC_TIMER_DIVIDE_128 }
    };

    for (uint32_t i = 0;
         i < sizeof(choices) / sizeof(choices[0]);
         i++) {

        uint64_t denominator =
            (uint64_t)choices[i].factor * frequency_hz;

        uint64_t count =
            clockevent_state.lapic_frequency_hz / denominator;

        if (count == 0 || count > UINT32_MAX) {
            continue;
        }

        *initial_count = (uint32_t)count;
        *divide_factor = choices[i].factor;
        *divide_encoding = choices[i].encoding;

        return true;
    }

    return false;
}


/*
 * Start the periodic LAPIC clockevent.
 */
clockevent_status_t clockevent_start_periodic(
    uint32_t frequency_hz
) {

    if (!clockevent_state.initialized) {
        return CLOCKEVENT_STATUS_NOT_INITIALIZED;
    }

    if (frequency_hz == 0) {
        return CLOCKEVENT_STATUS_INVALID_ARGUMENT;
    }

    if (clockevent_state.started) {
        return CLOCKEVENT_STATUS_ALREADY_STARTED;
    }

    uint32_t initial_count = 0;
    uint32_t divide_factor = 0;

    lapic_timer_divide_t divide_encoding;

    if (!clockevent_calculate_periodic_count(
            frequency_hz,
            &initial_count,
            &divide_factor,
            &divide_encoding)) {

        return CLOCKEVENT_STATUS_INVALID_ARGUMENT;
    }

    if (!lapic_timer_configure(
            LAPIC_TIMER_PERIODIC,
            divide_encoding,
            LAPIC_TIMER_VECTOR)) {

        return CLOCKEVENT_STATUS_TIMER_FAILURE;
    }

    if (!lapic_timer_start(initial_count)) {
        lapic_timer_stop();
        return CLOCKEVENT_STATUS_TIMER_FAILURE;
    }

    clockevent_state.periodic_frequency_hz =
        frequency_hz;

    clockevent_state.periodic_initial_count =
        initial_count;

    clockevent_state.divide_factor =
        divide_factor;

    clockevent_state.started = true;

    return CLOCKEVENT_STATUS_OK;
}


/*
 * Stop the periodic LAPIC clockevent.
 */
clockevent_status_t clockevent_stop(void) {

    if (!clockevent_state.initialized) {
        return CLOCKEVENT_STATUS_NOT_INITIALIZED;
    }

    if (!lapic_timer_stop()) {
        return CLOCKEVENT_STATUS_TIMER_FAILURE;
    }

    clockevent_state.started = false;

    clockevent_state.periodic_frequency_hz = 0;
    clockevent_state.periodic_initial_count = 0;
    clockevent_state.divide_factor = 0;

    return CLOCKEVENT_STATUS_OK;
}


/*
 * Retrieve clockevent information.
 */
bool clockevent_get_info(struct clockevent_info *info) {

    if (info == NULL ||
        !clockevent_state.initialized) {

        return false;
    }

    info->initialized = clockevent_state.initialized;
    info->calibrated = clockevent_state.lapic_frequency_hz != 0;
    info->started = clockevent_state.started;

    info->lapic_frequency_hz =
        clockevent_state.lapic_frequency_hz;

    info->periodic_frequency_hz =
        clockevent_state.periodic_frequency_hz;

    info->periodic_initial_count =
        clockevent_state.periodic_initial_count;

    info->divide_factor =
        clockevent_state.divide_factor;

    return true;
}

/*
 * Retrieve the number of LAPIC timer interrupts delivered.
 *
 * The generic clockevent layer exposes this statistic so
 * other kernel components do not need to access LAPIC directly.
 */
bool clockevent_get_interrupt_count(uint64_t *count) {

    if (count == NULL ||
        !clockevent_state.initialized) {

        return false;
    }

    struct lapic_info info = {0};

    if (!lapic_get_info(&info)) {
        return false;
    }

    *count = info.timer_interrupt_count;

    return true;
}
