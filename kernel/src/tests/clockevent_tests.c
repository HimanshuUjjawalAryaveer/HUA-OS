#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "clockevent_tests.h"

#include "time/clockevent.h"
#include "arch/x86_64/lapic.h"
#include "drivers/console/console.h"

static bool clockevent_test(
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


bool clockevent_run_tests(void) {

    bool all_passed = true;

    console_write("\n===== CLOCKEVENT TESTS =====\n");

    if (!clockevent_test(
            "Clockevent initialized",
            clockevent_is_initialized())) {

        return false;
    }

    struct clockevent_info info = {0};

    bool info_read =
        clockevent_get_info(&info);

    if (!clockevent_test(
            "Retrieve clockevent information",
            info_read)) {

        return false;
    }

    uint64_t interrupt_count_before = 0;
    uint64_t interrupt_count_after = 0;

    bool interrupt_count_read =
        clockevent_get_interrupt_count(
            &interrupt_count_before
        );

    if (!clockevent_test(
            "Retrieve LAPIC timer interrupt count",
            interrupt_count_read)) {

        all_passed = false;
    }

    bool second_interrupt_count_read =
        clockevent_get_interrupt_count(
            &interrupt_count_after
        );

    if (!clockevent_test(
            "Interrupt count remains readable",
            second_interrupt_count_read &&
            interrupt_count_after >= interrupt_count_before)) {

        all_passed = false;
    }

    if (!clockevent_test(
            "Reject NULL interrupt count pointer",
            !clockevent_get_interrupt_count(NULL))) {

        all_passed = false;
    }

    if (!clockevent_test(
            "LAPIC frequency calibrated",
            info.calibrated &&
            info.lapic_frequency_hz != 0 &&
            lapic_timer_is_calibrated())) {

        all_passed = false;
    }

    uint64_t lapic_frequency = 0;

    bool frequency_read =
        lapic_timer_get_calibrated_frequency(
            &lapic_frequency
        );

    if (!clockevent_test(
            "Read calibrated LAPIC frequency",
            frequency_read &&
            lapic_frequency == info.lapic_frequency_hz)) {

        all_passed = false;
    }

    if (!clockevent_test(
            "Calibration leaves timer stopped",
            !info.started)) {

        all_passed = false;
    }

    /*
     * Exercise a 100 Hz periodic timer configuration.
     * CPU interrupts remain disabled during this test.
     */
    clockevent_status_t start_status =
        clockevent_start_periodic(100);

    if (!clockevent_test(
            "Configure and start 100 Hz periodic timer",
            start_status == CLOCKEVENT_STATUS_OK)) {

        all_passed = false;
    }

    struct clockevent_info running_info = {0};

    bool running_info_read =
        clockevent_get_info(&running_info);

    if (!clockevent_test(
            "Periodic timer reports running state",
            running_info_read &&
            running_info.started &&
            running_info.periodic_frequency_hz == 100 &&
            running_info.periodic_initial_count != 0 &&
            running_info.divide_factor != 0)) {

        all_passed = false;
    }

    if (!clockevent_test(
            "Reject duplicate periodic start",
            clockevent_start_periodic(100) ==
                CLOCKEVENT_STATUS_ALREADY_STARTED)) {

        all_passed = false;
    }

    clockevent_status_t stop_status =
        clockevent_stop();

    if (!clockevent_test(
            "Stop periodic timer",
            stop_status == CLOCKEVENT_STATUS_OK)) {

        all_passed = false;
    }

    struct clockevent_info stopped_info = {0};

    bool stopped_info_read =
        clockevent_get_info(&stopped_info);

    if (!clockevent_test(
            "Periodic timer reports stopped state",
            stopped_info_read &&
            !stopped_info.started)) {

        all_passed = false;
    }

    if (!clockevent_test(
            "Reject zero periodic frequency",
            clockevent_start_periodic(0) ==
                CLOCKEVENT_STATUS_INVALID_ARGUMENT)) {

        all_passed = false;
    }

    if (all_passed) {
        console_set_color(0x36D399, 0x101820);
        console_write("All clockevent tests passed.\n");
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("Clockevent tests failed.\n");
    }

    console_set_color(0xFFFFFF, 0x101820);

    return all_passed;
}
