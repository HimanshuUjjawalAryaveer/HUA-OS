
#include "drivers/input/keyboard.h"

#include <stdint.h>

#include "arch/x86_64/io.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/interrupt_frame.h"
#include "interrupts/irq.h"

#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_COMMAND_PORT    0x64

#define PS2_STATUS_OUTPUT_FULL  (1U << 0)
#define PS2_STATUS_INPUT_FULL   (1U << 1)
#define PS2_STATUS_AUXILIARY    (1U << 5)
#define PS2_STATUS_TIMEOUT      (1U << 6)
#define PS2_STATUS_PARITY       (1U << 7)

#define PS2_COMMAND_DISABLE_FIRST_PORT  0xAD
#define PS2_COMMAND_ENABLE_FIRST_PORT   0xAE
#define PS2_COMMAND_READ_CONFIG         0x20
#define PS2_COMMAND_WRITE_CONFIG        0x60

#define PS2_CONFIG_IRQ1       (1U << 0)
#define PS2_CONFIG_TRANSLATE  (1U << 6)

#define KEYBOARD_COMMAND_DISABLE_SCANNING 0xF5
#define KEYBOARD_COMMAND_ENABLE_SCANNING  0xF4
#define KEYBOARD_RESPONSE_ACK             0xFA
#define KEYBOARD_RESPONSE_RESEND          0xFE

#define PS2_WAIT_LIMIT 100000U
#define KEYBOARD_COMMAND_RETRIES 3U

#define KEYBOARD_IRQ_ISA 1

/*
 * The buffer capacity is a power of two.
 * Head and tail are monotonically increasing counters.
 */
#define KEYBOARD_BUFFER_MASK (KEYBOARD_BUFFER_CAPACITY - 1U)

#if (KEYBOARD_BUFFER_CAPACITY == 0) || \
    ((KEYBOARD_BUFFER_CAPACITY & KEYBOARD_BUFFER_MASK) != 0)
#error "Keyboard buffer capacity must be a power of two"
#endif

static struct keyboard_event event_buffer[KEYBOARD_BUFFER_CAPACITY];

static volatile uint32_t event_head;
static volatile uint32_t event_tail;

static volatile uint64_t interrupt_count;
static volatile uint64_t scancode_count;
static volatile uint64_t event_count;
static volatile uint64_t dropped_count;

static bool initialized;
static bool active;

/* Decoder state. Only the keyboard interrupt handler modifies these. */
static bool left_shift_pressed;
static bool right_shift_pressed;
static bool caps_lock;
static bool caps_lock_down;
static bool extended_scancode;

static bool ps2_send_keyboard_command(uint8_t command);

/* Original US Set-1 keyboard character maps. */
static const char normal_map[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3',
    [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9',
    [0x0B] = '0',

    [0x0C] = '-', [0x0D] = '=',

    [0x0E] = '\b',
    [0x0F] = '\t',

    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
    [0x16] = 'u', [0x17] = 'i', [0x18] = 'o',
    [0x19] = 'p',

    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',

    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
    [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',

    [0x27] = ';', [0x28] = '\'',
    [0x29] = '`',

    [0x2B] = '\\',

    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',
    [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n',
    [0x32] = 'm',

    [0x33] = ',', [0x34] = '.',
    [0x35] = '/',

    [0x39] = ' '
};

static const char shifted_map[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#',
    [0x05] = '$', [0x06] = '%', [0x07] = '^',
    [0x08] = '&', [0x09] = '*', [0x0A] = '(',
    [0x0B] = ')',

    [0x0C] = '_', [0x0D] = '+',

    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y',
    [0x16] = 'U', [0x17] = 'I', [0x18] = 'O',
    [0x19] = 'P',

    [0x1A] = '{', [0x1B] = '}',

    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D',
    [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',

    [0x27] = ':', [0x28] = '"',
    [0x29] = '~',

    [0x2B] = '|',

    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C',
    [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N',
    [0x32] = 'M',

    [0x33] = '<', [0x34] = '>',
    [0x35] = '?'
};

/* --------------------------------------------------------- */
/* PS/2 controller helpers                                   */
/* --------------------------------------------------------- */

static bool ps2_wait_input_empty(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        if (!(x86_inb(PS2_STATUS_PORT) &
              PS2_STATUS_INPUT_FULL)) {
            return true;
        }
    }

    return false;
}

static bool ps2_wait_output_full(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        if (x86_inb(PS2_STATUS_PORT) &
            PS2_STATUS_OUTPUT_FULL) {
            return true;
        }
    }

    return false;
}

static void ps2_flush_output(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        uint8_t status = x86_inb(PS2_STATUS_PORT);

        if (!(status & PS2_STATUS_OUTPUT_FULL)) {
            break;
        }

        (void)x86_inb(PS2_DATA_PORT);
    }
}

static bool ps2_controller_command(uint8_t command) {
    if (!ps2_wait_input_empty()) {
        return false;
    }

    x86_outb(PS2_COMMAND_PORT, command);
    return true;
}

static bool ps2_read_config(uint8_t *config) {
    if (config == 0 ||
        !ps2_controller_command(PS2_COMMAND_READ_CONFIG) ||
        !ps2_wait_output_full()) {
        return false;
    }

    *config = x86_inb(PS2_DATA_PORT);
    return true;
}

static bool ps2_write_config(uint8_t config) {
    if (!ps2_controller_command(PS2_COMMAND_WRITE_CONFIG) ||
        !ps2_wait_input_empty()) {
        return false;
    }

    x86_outb(PS2_DATA_PORT, config);
    return ps2_wait_input_empty();
}

/*
 * Prepare the controller without enabling keyboard IRQ delivery.
 * The I/O APIC route is still masked at this stage.
 */
static bool ps2_controller_prepare(void) {
    if (!ps2_controller_command(
            PS2_COMMAND_DISABLE_FIRST_PORT)) {
        return false;
    }

    ps2_flush_output();

    uint8_t config;

    if (!ps2_read_config(&config)) {
        return false;
    }

    /*
     * Disable IRQ1 temporarily.
     * Enable controller translation so Set-2 keyboard data
     * is presented to the kernel as Set-1 scancodes.
     */
    config &= (uint8_t)~PS2_CONFIG_IRQ1;
    config |= PS2_CONFIG_TRANSLATE;

    if (!ps2_write_config(config)) {
        return false;
    }

    if (!ps2_controller_command(
            PS2_COMMAND_ENABLE_FIRST_PORT)) {
        return false;
    }

    /*
     * Disable scanning while the interrupt route is prepared.
     */
    return ps2_send_keyboard_command(
        KEYBOARD_COMMAND_DISABLE_SCANNING
    );
}

/*
 * Send a keyboard device command and wait for ACK.
 * RESEND is retried a bounded number of times.
 */
static bool ps2_send_keyboard_command(uint8_t command) {
    for (uint32_t attempt = 0;
         attempt < KEYBOARD_COMMAND_RETRIES;
         attempt++) {

        if (!ps2_wait_input_empty()) {
            return false;
        }

        x86_outb(PS2_DATA_PORT, command);

        if (!ps2_wait_output_full()) {
            return false;
        }

        uint8_t response = x86_inb(PS2_DATA_PORT);

        if (response == KEYBOARD_RESPONSE_ACK) {
            return true;
        }

        if (response != KEYBOARD_RESPONSE_RESEND) {
            return false;
        }
    }

    return false;
}

static bool ps2_enable_irq1(void) {
    uint8_t config;

    if (!ps2_read_config(&config)) {
        return false;
    }

    config |= PS2_CONFIG_IRQ1;

    return ps2_write_config(config);
}

/* --------------------------------------------------------- */
/* Event queue                                               */
/* --------------------------------------------------------- */

static bool keyboard_enqueue(
    enum keyboard_event_type type,
    char character
) {
    uint32_t head =
        __atomic_load_n(&event_head, __ATOMIC_RELAXED);

    uint32_t tail =
        __atomic_load_n(&event_tail, __ATOMIC_ACQUIRE);

    if ((uint32_t)(head - tail) >=
        KEYBOARD_BUFFER_CAPACITY) {

        __atomic_fetch_add(
            &dropped_count, 1, __ATOMIC_RELAXED
        );

        return false;
    }

    struct keyboard_event event = {
        .type = type,
        .character = character
    };

    event_buffer[head & KEYBOARD_BUFFER_MASK] = event;

    __atomic_store_n(
        &event_head,
        head + 1U,
        __ATOMIC_RELEASE
    );

    __atomic_fetch_add(
        &event_count, 1, __ATOMIC_RELAXED
    );

    return true;
}

bool keyboard_get_event(struct keyboard_event *event) {
    if (event == 0 || !initialized) {
        return false;
    }

    uint32_t tail =
        __atomic_load_n(&event_tail, __ATOMIC_RELAXED);

    uint32_t head =
        __atomic_load_n(&event_head, __ATOMIC_ACQUIRE);

    if (tail == head) {
        return false;
    }

    *event = event_buffer[tail & KEYBOARD_BUFFER_MASK];

    __atomic_store_n(
        &event_tail,
        tail + 1U,
        __ATOMIC_RELEASE
    );

    return true;
}

/* --------------------------------------------------------- */
/* Scancode decoding                                         */
/* --------------------------------------------------------- */

static char keyboard_scancode_to_ascii(uint8_t code) {
    if (code >= 128) {
        return 0;
    }

    bool shift =
        left_shift_pressed || right_shift_pressed;

    char character = normal_map[code];

    if (character >= 'a' && character <= 'z') {
        if (shift != caps_lock) {
            character = shifted_map[code];
        }
    } else if (shift && shifted_map[code] != 0) {
        character = shifted_map[code];
    }

    return character;
}

static void keyboard_decode_scancode(uint8_t code) {
    if (code == 0xE0) {
        extended_scancode = true;
        return;
    }

    bool extended = extended_scancode;
    extended_scancode = false;

    /*
     * Extended key releases are currently not needed by
     * the shell's navigation interface.
     */
    if (extended) {
        if (code & 0x80) {
            return;
        }

        switch (code) {
            case 0x48:
                keyboard_enqueue(KEY_EVENT_UP, 0);
                break;

            case 0x50:
                keyboard_enqueue(KEY_EVENT_DOWN, 0);
                break;

            case 0x49:
                keyboard_enqueue(KEY_EVENT_PAGE_UP, 0);
                break;

            case 0x51:
                keyboard_enqueue(KEY_EVENT_PAGE_DOWN, 0);
                break;

            default:
                break;
        }

        return;
    }

    bool released = (code & 0x80U) != 0;
    uint8_t key = code & 0x7FU;

    /* Left and right Shift have independent state. */
    if (key == 0x2A) {
        left_shift_pressed = !released;
        return;
    }

    if (key == 0x36) {
        right_shift_pressed = !released;
        return;
    }

    /* Toggle Caps Lock once per physical press. */
    if (key == 0x3A) {
        if (released) {
            caps_lock_down = false;
        } else if (!caps_lock_down) {
            caps_lock = !caps_lock;
            caps_lock_down = true;
        }

        return;
    }

    /* Ignore all other key-release events. */
    if (released) {
        return;
    }

    char character = keyboard_scancode_to_ascii(key);

    if (character != 0) {
        keyboard_enqueue(
            KEY_EVENT_CHARACTER,
            character
        );
    }
}

/*
 * Feed a synthetic scancode through the real decoder.
 * Used by keyboard unit tests before hardware activation.
 */
void keyboard_test_feed_scancode(uint8_t scancode) {
    __atomic_fetch_add(
        &scancode_count, 1, __ATOMIC_RELAXED
    );

    keyboard_decode_scancode(scancode);
}

/* --------------------------------------------------------- */
/* Interrupt handler                                         */
/* --------------------------------------------------------- */

static void keyboard_irq_handler(
    struct interrupt_frame *frame,
    void *context
) {
    (void)frame;
    (void)context;

    __atomic_fetch_add(
        &interrupt_count, 1, __ATOMIC_RELAXED
    );

    uint8_t status = x86_inb(PS2_STATUS_PORT);

    /*
     * IRQ1 should provide keyboard data, not mouse data.
     * Ignore corrupted or unavailable controller data.
     */
    if (!(status & PS2_STATUS_OUTPUT_FULL) ||
        (status & PS2_STATUS_AUXILIARY) ||
        (status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY))) {
        return;
    }

    uint8_t code = x86_inb(PS2_DATA_PORT);

    __atomic_fetch_add(
        &scancode_count, 1, __ATOMIC_RELAXED
    );

    keyboard_decode_scancode(code);
}

/* --------------------------------------------------------- */
/* Public driver interface                                   */
/* --------------------------------------------------------- */

bool keyboard_init(void) {
    if (active) {
        return false;
    }

    event_head = 0;
    event_tail = 0;

    interrupt_count = 0;
    scancode_count = 0;
    event_count = 0;
    dropped_count = 0;

    left_shift_pressed = false;
    right_shift_pressed = false;
    caps_lock = false;
    caps_lock_down = false;
    extended_scancode = false;

    initialized = true;

    return true;
}

keyboard_status_t keyboard_start(
    uint32_t destination_apic_id
) {
    if (!initialized) {
        return KEYBOARD_STATUS_NOT_INITIALIZED;
    }

    if (active) {
        return KEYBOARD_STATUS_ACTIVATION_FAILED;
    }

    if (!ps2_controller_prepare()) {
        return KEYBOARD_STATUS_CONTROLLER_FAILED;
    }

    /*
     * Register the handler before configuring or unmasking
     * the hardware route.
     */
    if (!irq_register_handler(
            KEYBOARD_IRQ_VECTOR,
            keyboard_irq_handler,
            0)) {

        return KEYBOARD_STATUS_IRQ_REGISTRATION_FAILED;
    }

    uint32_t keyboard_gsi = 0;

    ioapic_status_t route_status =
        ioapic_route_isa_irq_ex(
            KEYBOARD_IRQ_ISA,
            KEYBOARD_IRQ_VECTOR,
            destination_apic_id,
            &keyboard_gsi
        );

    if (route_status != IOAPIC_STATUS_OK) {
        irq_unregister_handler(KEYBOARD_IRQ_VECTOR);
        return KEYBOARD_STATUS_ROUTE_FAILED;
    }

    /*
     * The route remains masked here.
     * Enable controller IRQ1, then enable keyboard scanning.
     */
    if (!ps2_enable_irq1()) {
        ioapic_mask_gsi(keyboard_gsi);
        irq_unregister_handler(KEYBOARD_IRQ_VECTOR);
        return KEYBOARD_STATUS_ACTIVATION_FAILED;
    }

    if (!ps2_send_keyboard_command(
            KEYBOARD_COMMAND_ENABLE_SCANNING)) {

        uint8_t config;

        if (ps2_read_config(&config)) {
            config &= (uint8_t)~PS2_CONFIG_IRQ1;
            ps2_write_config(config);
        }

        ioapic_mask_gsi(keyboard_gsi);
        irq_unregister_handler(KEYBOARD_IRQ_VECTOR);

        return KEYBOARD_STATUS_ACTIVATION_FAILED;
    }

    /*
     * The generic IRQ handler is registered, the controller
     * is configured, and the device is scanning.
     */
    if (!ioapic_unmask_gsi(keyboard_gsi)) {
        uint8_t config;

        if (ps2_read_config(&config)) {
            config &= (uint8_t)~PS2_CONFIG_IRQ1;
            ps2_write_config(config);
        }

        ioapic_mask_gsi(keyboard_gsi);
        irq_unregister_handler(KEYBOARD_IRQ_VECTOR);

        return KEYBOARD_STATUS_ACTIVATION_FAILED;
    }

    active = true;

    return KEYBOARD_STATUS_OK;
}

bool keyboard_get_stats(struct keyboard_stats *stats) {
    if (stats == 0) {
        return false;
    }

    uint32_t head =
        __atomic_load_n(&event_head, __ATOMIC_ACQUIRE);

    uint32_t tail =
        __atomic_load_n(&event_tail, __ATOMIC_ACQUIRE);

    stats->interrupts =
        __atomic_load_n(&interrupt_count, __ATOMIC_RELAXED);

    stats->scancodes_received =
        __atomic_load_n(&scancode_count, __ATOMIC_RELAXED);

    stats->events_enqueued =
        __atomic_load_n(&event_count, __ATOMIC_RELAXED);

    stats->events_dropped =
        __atomic_load_n(&dropped_count, __ATOMIC_RELAXED);

    stats->queued_events = head - tail;
    stats->initialized = initialized;
    stats->active = active;

    return true;
}
