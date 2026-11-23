
#ifndef HUA_KEYBOARD_H
#define HUA_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#define KEYBOARD_IRQ_VECTOR 0x31
#define KEYBOARD_BUFFER_CAPACITY 128

enum keyboard_event_type {
    KEY_EVENT_CHARACTER,
    KEY_EVENT_UP,
    KEY_EVENT_DOWN,
    KEY_EVENT_PAGE_UP,
    KEY_EVENT_PAGE_DOWN
};

struct keyboard_event {
    enum keyboard_event_type type;
    char character;
};

typedef enum {
    KEYBOARD_STATUS_OK,
    KEYBOARD_STATUS_NOT_INITIALIZED,
    KEYBOARD_STATUS_INVALID_ARGUMENT,
    KEYBOARD_STATUS_CONTROLLER_FAILED,
    KEYBOARD_STATUS_IRQ_REGISTRATION_FAILED,
    KEYBOARD_STATUS_ROUTE_FAILED,
    KEYBOARD_STATUS_ACTIVATION_FAILED
} keyboard_status_t;

/* Initialize software state and the event queue. */
bool keyboard_init(void);

/* Initialize hardware and activate the keyboard interrupt route. */
keyboard_status_t keyboard_start(uint32_t destination_apic_id);

/* Retrieve the next event without polling hardware. */
bool keyboard_get_event(struct keyboard_event *event);

/* Retrieve keyboard driver statistics. */
struct keyboard_stats {
    uint64_t interrupts;
    uint64_t scancodes_received;
    uint64_t events_enqueued;
    uint64_t events_dropped;
    uint32_t queued_events;
    bool initialized;
    bool active;
};

bool keyboard_get_stats(struct keyboard_stats *stats);

/* Test support: inject a Set-1 scancode into the software decoder. */
void keyboard_test_feed_scancode(uint8_t scancode);

#endif
