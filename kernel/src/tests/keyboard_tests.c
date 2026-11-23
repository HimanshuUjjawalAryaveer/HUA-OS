
#include <stdbool.h>
#include <stdint.h>

#include "drivers/console/console.h"
#include "drivers/input/keyboard.h"
#include "keyboard_tests.h"

static bool keyboard_test_failure(const char *message) {
    console_set_color(0xFF5555, 0x101820);
    console_write("[ FAILED ] ");
    console_write(message);
    console_write("\n");
    console_set_color(0xFFFFFF, 0x101820);

    return false;
}

static void keyboard_test_success(const char *message) {
    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] ");
    console_write(message);
    console_write("\n");
    console_set_color(0xFFFFFF, 0x101820);
}

static bool keyboard_expect_character(char expected) {
    struct keyboard_event event;

    return keyboard_get_event(&event) &&
           event.type == KEY_EVENT_CHARACTER &&
           event.character == expected;
}

static bool keyboard_expect_event(
    enum keyboard_event_type expected
) {
    struct keyboard_event event;

    return keyboard_get_event(&event) &&
           event.type == expected;
}

bool keyboard_run_tests(void) {
    console_write("\n");
    console_write("===== KEYBOARD DRIVER TESTS =====\n");

    /*
     * Tests run before keyboard_start(), so no hardware IRQ
     * can modify the queue while synthetic scancodes are fed.
     */
    if (!keyboard_init()) {
        return keyboard_test_failure(
            "Keyboard test initialization"
        );
    }

    /*
     * TEST 1: Empty queue.
     */
    struct keyboard_event event;

    if (keyboard_get_event(&event)) {
        return keyboard_test_failure(
            "Empty event queue"
        );
    }

    keyboard_test_success("Empty event queue");

    /*
     * TEST 2: Reject a NULL event pointer.
     */
    if (keyboard_get_event(0)) {
        return keyboard_test_failure(
            "NULL event pointer rejection"
        );
    }

    keyboard_test_success("NULL event pointer rejection");

    /*
     * TEST 3: Normal lowercase character.
     * A press: 0x1E. A release: 0x9E.
     */
    keyboard_test_feed_scancode(0x1E);
    keyboard_test_feed_scancode(0x9E);

    if (!keyboard_expect_character('a')) {
        return keyboard_test_failure(
            "Lowercase scancode decoding"
        );
    }

    keyboard_test_success("Lowercase scancode decoding");

    /*
     * TEST 4: Shift produces uppercase.
     */
    keyboard_test_feed_scancode(0x2A); /* Left Shift down */
    keyboard_test_feed_scancode(0x1E); /* A down */
    keyboard_test_feed_scancode(0xAA); /* Left Shift up */

    if (!keyboard_expect_character('A')) {
        return keyboard_test_failure(
            "Shift modifier decoding"
        );
    }

    keyboard_test_success("Shift modifier decoding");

    /*
     * TEST 5: Caps Lock produces uppercase.
     */
    keyboard_test_feed_scancode(0x3A); /* Caps Lock down */
    keyboard_test_feed_scancode(0xBA); /* Caps Lock up */
    keyboard_test_feed_scancode(0x1E); /* A down */

    if (!keyboard_expect_character('A')) {
        return keyboard_test_failure(
            "Caps Lock decoding"
        );
    }

    keyboard_test_success("Caps Lock decoding");

    /*
     * TEST 6: Shift + Caps Lock produces lowercase.
     */
    keyboard_test_feed_scancode(0x2A); /* Left Shift down */
    keyboard_test_feed_scancode(0x1E); /* A down */
    keyboard_test_feed_scancode(0xAA); /* Left Shift up */

    if (!keyboard_expect_character('a')) {
        return keyboard_test_failure(
            "Shift and Caps Lock interaction"
        );
    }

    keyboard_test_success(
        "Shift and Caps Lock interaction"
    );

    /*
     * TEST 7: Extended navigation keys.
     */
    keyboard_test_feed_scancode(0xE0);
    keyboard_test_feed_scancode(0x48); /* Up */

    keyboard_test_feed_scancode(0xE0);
    keyboard_test_feed_scancode(0x50); /* Down */

    keyboard_test_feed_scancode(0xE0);
    keyboard_test_feed_scancode(0x49); /* Page Up */

    keyboard_test_feed_scancode(0xE0);
    keyboard_test_feed_scancode(0x51); /* Page Down */

    if (!keyboard_expect_event(KEY_EVENT_UP) ||
        !keyboard_expect_event(KEY_EVENT_DOWN) ||
        !keyboard_expect_event(KEY_EVENT_PAGE_UP) ||
        !keyboard_expect_event(KEY_EVENT_PAGE_DOWN)) {

        return keyboard_test_failure(
            "Extended navigation scancode decoding"
        );
    }

    keyboard_test_success(
        "Extended navigation scancode decoding"
    );

    /*
     * TEST 8: Scancode statistics.
     */
    struct keyboard_stats stats;

    if (!keyboard_get_stats(&stats)) {
        return keyboard_test_failure(
            "Keyboard statistics retrieval"
        );
    }

    if (stats.scancodes_received != 19 ||
        stats.events_enqueued != 8 ||
        stats.events_dropped != 0 ||
        stats.queued_events != 0 ||
        !stats.initialized ||
        stats.active) {

        return keyboard_test_failure(
            "Keyboard statistics accuracy"
        );
    }

    keyboard_test_success("Keyboard statistics accuracy");

    /*
     * TEST 9: NULL statistics pointer.
     */
    if (keyboard_get_stats(0)) {
        return keyboard_test_failure(
            "NULL statistics pointer rejection"
        );
    }

    keyboard_test_success(
        "NULL statistics pointer rejection"
    );

    /*
     * TEST 10: Fill the complete event queue.
     */
    if (!keyboard_init()) {
        return keyboard_test_failure(
            "Queue overflow test initialization"
        );
    }

    for (uint32_t i = 0;
         i < KEYBOARD_BUFFER_CAPACITY;
         i++) {

        keyboard_test_feed_scancode(0x1E);
    }

    if (!keyboard_get_stats(&stats) ||
        stats.queued_events != KEYBOARD_BUFFER_CAPACITY ||
        stats.events_enqueued != KEYBOARD_BUFFER_CAPACITY ||
        stats.events_dropped != 0) {

        return keyboard_test_failure(
            "Event queue capacity"
        );
    }

    keyboard_test_success("Event queue capacity");

    /*
     * TEST 11: An additional event must be dropped safely.
     */
    keyboard_test_feed_scancode(0x30); /* B */

    if (!keyboard_get_stats(&stats) ||
        stats.queued_events != KEYBOARD_BUFFER_CAPACITY ||
        stats.events_dropped != 1) {

        return keyboard_test_failure(
            "Full queue overflow handling"
        );
    }

    keyboard_test_success("Full queue overflow handling");

    /*
     * TEST 12: Drain all queued events in FIFO order.
     */
    for (uint32_t i = 0;
         i < KEYBOARD_BUFFER_CAPACITY;
         i++) {

        if (!keyboard_expect_character('a')) {
            return keyboard_test_failure(
                "Event queue FIFO ordering"
            );
        }
    }

    if (keyboard_get_event(&event)) {
        return keyboard_test_failure(
            "Event queue empty after draining"
        );
    }

    if (!keyboard_get_stats(&stats) ||
        stats.queued_events != 0 ||
        stats.events_enqueued != KEYBOARD_BUFFER_CAPACITY ||
        stats.events_dropped != 1) {

        return keyboard_test_failure(
            "Queue statistics after draining"
        );
    }

    keyboard_test_success(
        "Event queue FIFO ordering and draining"
    );

    /*
     * Restore clean state before real hardware activation.
     * No synthetic events or modifier state remain.
     */
    if (!keyboard_init()) {
        return keyboard_test_failure(
            "Keyboard state restoration"
        );
    }

    keyboard_test_success(
        "Keyboard state restored for hardware activation"
    );

    console_write("===== KEYBOARD TESTS PASSED =====\n\n");

    return true;
}
