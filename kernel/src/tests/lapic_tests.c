#include <stdint.h>
#include <stdbool.h>

#include "lapic_tests.h"

#include "arch/x86_64/lapic.h"
#include "drivers/console/console.h"

static bool lapic_test(
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


bool lapic_run_tests(void) {

    bool all_passed = true;

    console_write("\n===== LOCAL APIC TESTS =====\n");

    struct lapic_info info = {0};

    bool initialized =
        lapic_is_initialized();

    if (!lapic_test(
            "Local APIC initialized",
            initialized)) {

        return false;
    }

    if (!lapic_test(
            "Retrieve LAPIC information",
            lapic_get_info(&info))) {

        return false;
    }

    if (!lapic_test(
            "APIC globally enabled",
            info.globally_enabled)) {

        all_passed = false;
    }

    if (!lapic_test(
            "APIC supported by CPU",
            info.supported)) {

        all_passed = false;
    }

    if (!lapic_test(
            "Valid APIC version",
            info.version != 0)) {

        all_passed = false;
    }

    /*
     * Read the APIC ID register.
     */
    uint64_t id_register = 0;

    bool id_read =
        lapic_read_register(
            LAPIC_REG_ID,
            &id_register
        );

    uint32_t observed_id =
        info.x2apic_mode
            ? (uint32_t)id_register
            : (uint32_t)(id_register >> 24);

    if (!lapic_test(
            "Read Local APIC ID register",
            id_read && observed_id == info.apic_id)) {

        all_passed = false;
    }

    /*
     * Read the APIC version register.
     */
    uint64_t version_register = 0;

    bool version_read =
        lapic_read_register(
            LAPIC_REG_VERSION,
            &version_register
        );

    if (!lapic_test(
            "Read Local APIC version register",
            version_read &&
            ((uint32_t)version_register & 0xFFU) ==
                info.version)) {

        all_passed = false;
    }

    /*
     * Configure the timer but leave it masked.
     *
     * We deliberately do not start it because global
     * interrupt delivery is not enabled yet.
     */
    bool timer_configured =
        lapic_timer_configure(
            LAPIC_TIMER_ONE_SHOT,
            LAPIC_TIMER_DIVIDE_16,
            LAPIC_TIMER_VECTOR
        );

    if (!lapic_test(
            "Configure masked one-shot timer",
            timer_configured)) {

        all_passed = false;
    }

    bool timer_stopped =
        lapic_timer_stop();

    if (!lapic_test(
            "Keep LAPIC timer stopped",
            timer_stopped)) {

        all_passed = false;
    }

    uint32_t current_count = 0;

    bool count_read =
        lapic_timer_get_current_count(
            &current_count
        );

    if (!lapic_test(
            "Read LAPIC timer current count",
            count_read)) {

        all_passed = false;
    }

    /*
     * Timer calibration is intentionally deferred.
     */
    if (!lapic_test(
            "Timer not falsely reported as calibrated",
            !lapic_timer_is_calibrated())) {

        all_passed = false;
    }

    uint32_t error_status = 0;

    if (!lapic_test(
            "Read LAPIC error status",
            lapic_get_error_status(&error_status))) {

        all_passed = false;
    }

    if (all_passed) {
        console_set_color(0x36D399, 0x101820);
        console_write("All Local APIC tests passed.\n");
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("Local APIC tests failed.\n");
    }

    console_set_color(0xFFFFFF, 0x101820);

    return all_passed;
}
