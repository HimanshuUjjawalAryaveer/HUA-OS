
#include "ioapic_tests.h"

#include <stdint.h>

#include "arch/x86_64/ioapic.h"
#include "drivers/console/console.h"

static bool ioapic_test(
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

bool ioapic_run_tests(void) {

    bool all_passed = true;

    console_write("\n===== I/O APIC TESTS =====\n");

    if (!ioapic_test(
            "I/O APIC initialized",
            ioapic_is_initialized())) {
        return false;
    }

    uint32_t count =
        ioapic_get_controller_count();

    if (!ioapic_test(
            "At least one I/O APIC discovered",
            count > 0 &&
            count <= IOAPIC_MAX_CONTROLLERS)) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {

        struct ioapic_controller_info info = {0};

        bool retrieved =
            ioapic_get_controller_info(i, &info);

        if (!ioapic_test(
                "Retrieve controller information",
                retrieved)) {

            all_passed = false;
            continue;
        }

        if (!ioapic_test(
                "Controller has valid version",
                info.version != 0)) {

            all_passed = false;
        }

        if (!ioapic_test(
                "Controller has redirection entries",
                info.redirection_count > 0)) {

            all_passed = false;
        }

        if (!ioapic_test(
                "Controller has aligned MMIO mapping",
                (info.virtual_address & 0xFFFU) == 0)) {

            all_passed = false;
        }

        /*
         * Every entry must remain masked after initialization.
         */
        bool all_masked = true;

        for (uint32_t pin = 0;
            pin < info.redirection_count;
            pin++) {

            uint32_t gsi = info.gsi_base + pin;

            struct ioapic_route_info route = {0};

            if (!ioapic_get_route_info(gsi, &route) ||
                !route.masked) {

                all_masked = false;
                break;
            }
        }

        if (!ioapic_test(
                "All redirection entries remain masked",
                all_masked)) {

            all_passed = false;
        }
    }

    if (!ioapic_test(
            "Reject invalid ISA IRQ",
            ioapic_route_isa_irq(
                16,
                0x50,
                0
            ) == IOAPIC_STATUS_INVALID_ARGUMENT)) {

        all_passed = false;
    }

    if (!ioapic_test(
            "Reject unmasking an unconfigured GSI",
            !ioapic_unmask_gsi(UINT32_MAX))) {

        all_passed = false;
    }

    if (all_passed) {
        console_set_color(0x36D399, 0x101820);
        console_write("All I/O APIC tests passed.\n");
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("I/O APIC tests failed.\n");
    }

    console_set_color(0xFFFFFF, 0x101820);

    return all_passed;
}
