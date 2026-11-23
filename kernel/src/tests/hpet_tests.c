
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "hpet_tests.h"

#include "arch/x86_64/hpet.h"
#include "drivers/console/console.h"

#define HPET_TEST_MAX_POLLS 20000000ULL

/*
 * Print one HPET test result.
 */
static bool hpet_test(
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


/*
 * Wait until HPET advances by approximately one millisecond.
 *
 * The loop is bounded so a broken or stopped HPET cannot
 * hang the kernel indefinitely.
 */
static bool hpet_wait_for_counter_advance(
    uint64_t initial,
    uint64_t required_ticks
) {
    for (uint64_t i = 0; i < HPET_TEST_MAX_POLLS; i++) {

        uint64_t current = 0;

        if (hpet_read_counter(&current) != HPET_STATUS_OK) {
            return false;
        }

        if (current >= initial &&
            current - initial >= required_ticks) {

            return true;
        }
    }

    return false;
}


bool hpet_run_tests(void) {

    bool all_passed = true;

    console_write("\n===== HPET TESTS =====\n");

    /*
     * Verify successful initialization.
     */
    if (!hpet_test(
            "HPET initialized",
            hpet_is_initialized())) {

        return false;
    }

    /*
     * Retrieve published HPET information.
     */
    const struct hpet_info *info =
        hpet_get_info();

    if (!hpet_test(
            "Retrieve HPET information",
            info != NULL)) {

        return false;
    }

    if (!hpet_test(
            "HPET main counter is 64-bit",
            info->counter_64_bit)) {

        all_passed = false;
    }

    if (!hpet_test(
            "HPET main counter enabled",
            info->counter_enabled)) {

        all_passed = false;
    }

    if (!hpet_test(
            "Valid HPET physical address",
            info->physical_address != 0 &&
            (info->physical_address & 0xFFFULL) == 0)) {

        all_passed = false;
    }

    if (!hpet_test(
            "Valid HPET counter period",
            info->period_femtoseconds >= 1000000ULL &&
            info->period_femtoseconds <= 100000000ULL)) {

        all_passed = false;
    }

    if (!hpet_test(
            "Valid HPET frequency",
            info->frequency_hz != 0 &&
            info->frequency_hz ==
                1000000000000000ULL /
                info->period_femtoseconds)) {

        all_passed = false;
    }

    /*
     * Read the main counter.
     */
    uint64_t first_counter = 0;

    bool first_read =
        hpet_read_counter(&first_counter) == HPET_STATUS_OK;

    if (!hpet_test(
            "Read HPET main counter",
            first_read)) {

        all_passed = false;
    }

    /*
     * Verify that the hardware counter actually advances.
     */
    uint64_t second_counter = 0;

    bool second_read =
        hpet_read_counter(&second_counter) == HPET_STATUS_OK;

    if (!hpet_test(
            "Read HPET counter a second time",
            second_read)) {

        all_passed = false;
    }

    if (!hpet_test(
            "HPET counter is non-decreasing",
            first_read &&
            second_read &&
            second_counter >= first_counter)) {

        all_passed = false;
    }

    /*
     * One millisecond expressed in femtoseconds:
     *
     * 1 ms = 10^12 femtoseconds.
     *
     * Required ticks = ceil(10^12 / period_fs).
     */
    uint64_t required_ticks =
        (1000000000000ULL +
         info->period_femtoseconds - 1ULL) /
        info->period_femtoseconds;

    bool advancing =
        first_read &&
        hpet_wait_for_counter_advance(
            first_counter,
            required_ticks
        );

    if (!hpet_test(
            "HPET hardware counter advances",
            advancing)) {

        all_passed = false;
    }

    /*
     * Invalid argument handling.
     */
    if (!hpet_test(
            "Reject NULL counter output",
            hpet_read_counter(NULL) ==
                HPET_STATUS_INVALID_ARGUMENT)) {

        all_passed = false;
    }

    /*
     * Verify that the information getter is stable.
     */
    if (!hpet_test(
            "HPET information remains available",
            hpet_get_info() == info)) {

        all_passed = false;
    }

    if (all_passed) {
        console_set_color(0x36D399, 0x101820);
        console_write("All HPET tests passed.\n");
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("HPET tests failed.\n");
    }

    console_set_color(0xFFFFFF, 0x101820);

    return all_passed;
}
