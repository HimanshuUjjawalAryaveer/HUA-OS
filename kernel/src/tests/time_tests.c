#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "time_tests.h"
#include "time/time.h"
#include "time/clocksource.h"
#include "drivers/console/console.h"

#define TIME_TEST_MAX_POLLS 20000000ULL

static bool time_test(
    const char *name,
    bool passed
) {

    if (passed) {
        console_set_color(0x36D399, 0x101820);
        console_write("[ PASS ] ");
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAIL ] ");
    }

    console_set_color(0xFFFFFF, 0x101820);

    console_write(name);
    console_write("\n");

    return passed;
}


bool time_run_tests(void) {

    bool all_passed = true;

    console_write("\n===== TIMEKEEPING TESTS =====\n");

    if (!time_test(
            "Timekeeping initialized",
            time_is_initialized())) {

        return false;
    }

    if (!time_test(
            "Clocksource initialized",
            clocksource_is_initialized())) {

        return false;
    }

    const struct clocksource_info *info =
        clocksource_get_info();

    if (!time_test(
            "Retrieve clocksource information",
            info != NULL)) {

        return false;
    }

    if (!time_test(
            "HPET selected as clocksource",
            info->name != NULL &&
            info->frequency_hz != 0 &&
            info->period_femtoseconds != 0)) {

        all_passed = false;
    }

    if (!time_test(
            "Reject NULL monotonic-time output",
            time_get_monotonic_ns(NULL) ==
                TIME_STATUS_INVALID_ARGUMENT)) {

        all_passed = false;
    }

    uint64_t first = 0;
    uint64_t second = 0;

    bool first_read =
        time_get_monotonic_ns(&first) == TIME_STATUS_OK;

    bool second_read =
        time_get_monotonic_ns(&second) == TIME_STATUS_OK;

    if (!time_test(
            "Read monotonic time",
            first_read && second_read)) {

        all_passed = false;
    }

    if (!time_test(
            "Monotonic time does not go backwards",
            first_read && second_read && second >= first)) {

        all_passed = false;
    }

    /*
     * Verify that time advances by at least 1 millisecond.
     */
    bool advanced = false;

    if (second_read) {

        for (uint64_t i = 0;
             i < TIME_TEST_MAX_POLLS;
             i++) {

            uint64_t current = 0;

            if (time_get_monotonic_ns(&current) != TIME_STATUS_OK) {
                break;
            }

            if (current >= second &&
                current - second >= 1000000ULL) {

                advanced = true;
                break;
            }
        }
    }

    if (!time_test(
            "Monotonic clock advances",
            advanced)) {

        all_passed = false;
    }

    /*
    * Test uptime conversions.
    *
    * Each uptime function reads the hardware clock independently.
    * Therefore, bracket each read with monotonic timestamps instead
    * of expecting two separate reads to return identical values.
    */

    uint64_t before_ns = 0;
    uint64_t after_ns = 0;

    bool before_read =
        time_get_monotonic_ns(&before_ns) == TIME_STATUS_OK;

    uint64_t uptime_us = time_get_uptime_us();

    bool after_read =
        time_get_monotonic_ns(&after_ns) == TIME_STATUS_OK;

    if (!time_test(
            "Uptime microseconds available",
            before_read &&
            after_read &&
            after_ns >= before_ns &&
            uptime_us >= before_ns / 1000ULL &&
            uptime_us <= after_ns / 1000ULL)) {

        all_passed = false;
    }


    /* Milliseconds */

    before_read =
        time_get_monotonic_ns(&before_ns) == TIME_STATUS_OK;

    uint64_t uptime_ms = time_get_uptime_ms();

    after_read =
        time_get_monotonic_ns(&after_ns) == TIME_STATUS_OK;

    if (!time_test(
            "Uptime milliseconds available",
            before_read &&
            after_read &&
            after_ns >= before_ns &&
            uptime_ms >= before_ns / 1000000ULL &&
            uptime_ms <= after_ns / 1000000ULL)) {

        all_passed = false;
    }


    /* Seconds */

    before_read =
        time_get_monotonic_ns(&before_ns) == TIME_STATUS_OK;

    uint64_t uptime_seconds = time_get_uptime_seconds();

    after_read =
        time_get_monotonic_ns(&after_ns) == TIME_STATUS_OK;

    if (!time_test(
            "Uptime seconds available",
            before_read &&
            after_read &&
            after_ns >= before_ns &&
            uptime_seconds >= before_ns / 1000000000ULL &&
            uptime_seconds <= after_ns / 1000000000ULL)) {

        all_passed = false;
    }

    if (!time_test(
            "Reject duplicate time initialization",
            time_init() == TIME_STATUS_ALREADY_INITIALIZED)) {

        all_passed = false;
    }

    console_write("\n");

    if (all_passed) {
        console_set_color(0x36D399, 0x101820);
        console_write("[ PASS ] All timekeeping tests passed\n");
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAIL ] Timekeeping tests failed\n");
    }

    console_set_color(0xFFFFFF, 0x101820);

    return all_passed;
}
