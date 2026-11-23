
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "tsc_tests.h"

#include "arch/x86_64/tsc.h"
#include "drivers/console/console.h"

static bool tsc_test(
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

bool tsc_run_tests(void) {

    bool all_passed = true;

    console_write("\n===== TSC TESTS =====\n");

    struct tsc_info info = {0};

    if (!tsc_test(
            "TSC initialization completed",
            tsc_is_initialized())) {

        return false;
    }

    if (!tsc_test(
            "Retrieve TSC information",
            tsc_get_info(&info))) {

        return false;
    }

    if (!info.supported) {

        /*
         * TSC is optional for this driver. If unavailable,
         * verify that reads and frequency queries fail safely.
         */
        uint64_t value = 0;
        uint64_t frequency = 0;

        bool read_rejected =
            !tsc_read(&value);

        bool frequency_rejected =
            !tsc_get_frequency(&frequency);

        if (!tsc_test(
                "Reject TSC read when unsupported",
                read_rejected)) {

            all_passed = false;
        }

        if (!tsc_test(
                "Reject frequency query when unsupported",
                frequency_rejected)) {

            all_passed = false;
        }

        return all_passed;
    }

    if (!tsc_test(
            "CPU reports TSC support",
            info.supported)) {

        all_passed = false;
    }

    if (!tsc_test(
            "Invariant TSC state is consistent",
            !info.invariant || info.supported)) {

        all_passed = false;
    }

    /*
     * Read the counter multiple times.
     *
     * We only require non-decreasing values. We do not
     * assume a particular CPU frequency or elapsed time.
     */
    uint64_t first = 0;
    uint64_t second = 0;
    uint64_t third = 0;

    bool first_read = tsc_read(&first);
    bool second_read = tsc_read(&second);
    bool third_read = tsc_read(&third);

    if (!tsc_test(
            "Read TSC counter",
            first_read && second_read && third_read)) {

        all_passed = false;
    }

    if (!tsc_test(
            "TSC counter is non-decreasing",
            first_read &&
            second_read &&
            third_read &&
            second <= third &&
            first <= second)) {

        all_passed = false;
    }

    /*
     * A NULL output pointer must be rejected.
     */
    if (!tsc_test(
            "Reject NULL TSC read output",
            !tsc_read(NULL))) {

        all_passed = false;
    }

    /*
     * Frequency is optional. If available, it must be
     * nonzero and agree with the reported information.
     */
    uint64_t frequency = 0;

    bool frequency_available =
        tsc_get_frequency(&frequency);

    if (!tsc_test(
            "CPUID frequency state is consistent",
            frequency_available
                ? (frequency != 0 &&
                   info.frequency_available &&
                   frequency == info.frequency_hz)
                : (!info.frequency_available &&
                   info.frequency_hz == 0))) {

        all_passed = false;
    }

    /*
     * NULL output pointers must be rejected.
     */
    if (!tsc_test(
            "Reject NULL frequency output",
            !tsc_get_frequency(NULL))) {

        all_passed = false;
    }

    if (!tsc_test(
            "Reject NULL information output",
            !tsc_get_info(NULL))) {

        all_passed = false;
    }

    /*
     * Initialization is idempotent.
     */
    tsc_status_t repeated_init =
        tsc_init();

    if (!tsc_test(
            "TSC initialization is idempotent",
            repeated_init == TSC_STATUS_OK)) {

        all_passed = false;
    }

    if (all_passed) {
        console_set_color(0x36D399, 0x101820);
        console_write("All TSC tests passed.\n");
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("TSC tests failed.\n");
    }

    console_set_color(0xFFFFFF, 0x101820);

    return all_passed;
}
