#include "shell/shell.h"

#include "drivers/console/console.h"
#include <stdbool.h>
#include <stdint.h>

#include "time/clockevent.h"
#include "time/time.h"

#define SHELL_BUFFER_SIZE 256

static char input_buffer[SHELL_BUFFER_SIZE];
static unsigned int input_length = 0;

/*
 * Print an unsigned 64-bit integer in decimal.
 */
static void shell_write_uint64(uint64_t value) {

    char buffer[21];

    unsigned int position = sizeof(buffer);

    buffer[--position] = '\0';

    do {
        buffer[--position] =
            (char)('0' + (value % 10));

        value /= 10;

    } while (value != 0);

    console_write(&buffer[position]);
}

/*
 * Compare two strings.
 * Returns true only if both strings are identical.
 */
static int shell_string_equals(
    const char *a,
    const char *b
) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }

        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

/*
 * Check whether a string begins with a prefix.
 */
static int shell_starts_with(
    const char *text,
    const char *prefix
) {
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return 0;
        }

        text++;
        prefix++;
    }

    return 1;
}

/*
 * Display the command prompt.
 */
static void shell_print_prompt(void) {
    console_set_color(0x36BFFF, 0x101820);
    console_write("root@hua");

    console_set_color(0xFFFFFF, 0x101820);
    console_write(":~# ");
}

/*
 * Display available commands.
 */
static void shell_help(void) {
    console_write("Available commands:\n");
    console_write("  help       - Show available commands\n");
    console_write("  clear      - Clear the console\n");
    console_write("  about      - About HUA OS\n");
    console_write("  version    - Show OS version\n");
    console_write("  echo TEXT  - Print TEXT\n");
    console_write("  timer      - Show timer information and measured frequency\n");
}

/*
 * Check whether CPU interrupts are currently enabled.
 */
static bool shell_interrupts_enabled(void) {

    uint64_t rflags;

    asm volatile (
        "pushfq\n"
        "popq %0"
        : "=r"(rflags)
        :
        : "memory"
    );

    return (rflags & (1ULL << 9)) != 0;
}


/*
 * Display timer configuration and measure actual
 * LAPIC timer interrupt delivery over approximately
 * one second.
 */
static void shell_timer(void) {

    struct clockevent_info info = {0};

    if (!clockevent_get_info(&info) ||
        !info.initialized ||
        !info.started) {

        console_write("Timer is not running.\n");
        return;
    }

    if (!shell_interrupts_enabled()) {
        console_write(
            "Cannot measure timer: CPU interrupts are disabled.\n"
        );

        return;
    }

    uint64_t initial_count = 0;

    if (!clockevent_get_interrupt_count(&initial_count)) {
        console_write(
            "Unable to read timer interrupt counter.\n"
        );

        return;
    }

    uint64_t start_ns = 0;
    uint64_t current_ns = 0;

    if (time_get_monotonic_ns(&start_ns) != TIME_STATUS_OK) {
        console_write("Unable to read monotonic time.\n");
        return;
    }

    current_ns = start_ns;

    bool measurement_complete = false;

    /*
     * Wait approximately one second using the HPET-backed
     * monotonic clock.
     *
     * CPU interrupts remain enabled during this loop,
     * allowing LAPIC timer interrupts to be delivered.
     */
    for (uint64_t i = 0;
         i < 100000000ULL;
         i++) {

        if (time_get_monotonic_ns(&current_ns) != TIME_STATUS_OK) {
            console_write("Unable to read monotonic time.\n");
            return;
        }

        if (current_ns - start_ns >= 1000000000ULL) {
            measurement_complete = true;
            break;
        }
    }

    if (!measurement_complete) {
        console_write(
            "Timer measurement timed out.\n"
        );

        return;
    }

    uint64_t final_count = 0;

    if (!clockevent_get_interrupt_count(&final_count)) {
        console_write(
            "Unable to read final timer interrupt counter.\n"
        );

        return;
    }

    uint64_t elapsed_ns =
        current_ns - start_ns;

    if (final_count < initial_count || elapsed_ns == 0) {
        console_write(
            "Invalid timer measurement.\n"
        );

        return;
    }

    uint64_t delivered_interrupts =
        final_count - initial_count;

    /*
     * Calculate:
     *
     * measured frequency =
     * delivered interrupts * 1,000,000,000 / elapsed ns
     *
     * Check multiplication before performing it.
     */
    if (delivered_interrupts >
        UINT64_MAX / 1000000000ULL) {

        console_write(
            "Timer measurement overflow.\n"
        );

        return;
    }

    uint64_t measured_frequency =
        (delivered_interrupts * 1000000000ULL) /
        elapsed_ns;

    console_write("\n===== HUA TIMER INFORMATION =====\n");

    console_write("Clockevent: LAPIC\n");

    console_write("Clocksource: HPET\n");

    console_write("Configured frequency: ");
    shell_write_uint64(info.periodic_frequency_hz);
    console_write(" Hz\n");

    console_write("LAPIC calibrated frequency: ");
    shell_write_uint64(info.lapic_frequency_hz);
    console_write(" Hz\n");

    console_write("LAPIC divide factor: ");
    shell_write_uint64(info.divide_factor);
    console_write("\n");

    console_write("Total delivered interrupts: ");
    shell_write_uint64(final_count);
    console_write("\n");

    console_write("Interrupts during measurement: ");
    shell_write_uint64(delivered_interrupts);
    console_write("\n");

    console_write("Measurement duration: ");
    shell_write_uint64(elapsed_ns / 1000000ULL);
    console_write(" ms\n");

    console_write("Measured interrupt frequency: ");
    shell_write_uint64(measured_frequency);
    console_write(" Hz\n");

    console_write("=================================\n");
}

/*
 * Execute a completed command.
 */
static void shell_execute(const char *command) {

    if (shell_string_equals(command, "")) {
        return;
    }

    if (shell_string_equals(command, "help")) {
        shell_help();
    }

    else if (shell_string_equals(command, "clear")) {
        console_clear();
    }

    else if (shell_string_equals(command, "about")) {
        console_write("HUA OS\n");
        console_write("An educational x86_64 operating system.\n");
        console_write("Built using C and NASM Assembly.\n");
        console_write("Designed with a modular monolithic kernel.\n");
    }

    else if (shell_string_equals(command, "version")) {
        console_write("HUA OS version 0.1.0\n");
    }

    else if (shell_string_equals(command, "timer")) {
        shell_timer();
    }

    else if (shell_starts_with(command, "echo ")) {
        console_write(command + 5);
        console_put_char('\n');
    }

    else {
        console_write("Unknown command: ");
        console_write(command);
        console_put_char('\n');
    }
}

/*
 * Initialize the shell.
 */
void shell_init(void) {
    input_length = 0;
    input_buffer[0] = '\0';

    console_set_color(0x36BFFF, 0x101820);
    console_write("HUA OS Shell\n");

    console_set_color(0xFFFFFF, 0x101820);
    console_write("Type 'help' to see available commands.\n\n");

    shell_print_prompt();
}

/*
 * Process one character received from the keyboard.
 */
void shell_handle_character(char character) {

    /*
     * ENTER:
     * Finish the current command and execute it.
     */
    if (character == '\n') {

        console_put_char('\n');

        input_buffer[input_length] = '\0';

        shell_execute(input_buffer);

        input_length = 0;
        input_buffer[0] = '\0';

        shell_print_prompt();

        return;
    }

    /*
     * BACKSPACE:
     * Remove one character from the input buffer.
     */
    if (character == '\b') {

        if (input_length > 0) {
            input_length--;

            input_buffer[input_length] = '\0';

            console_backspace();
        }

        return;
    }

    /*
     * TAB:
     * Insert four spaces into the input buffer.
     */
    if (character == '\t') {

        if (input_length + 4 < SHELL_BUFFER_SIZE) {
            for (int i = 0; i < 4; i++) {
                input_buffer[input_length++] = ' ';
                console_put_char(' ');
            }

            input_buffer[input_length] = '\0';
        }

        return;
    }

    /*
     * Printable ASCII characters:
     * Store the character and display it.
     */
    if (character >= 32 && character <= 126) {

        if (input_length < SHELL_BUFFER_SIZE - 1) {
            input_buffer[input_length++] = character;

            input_buffer[input_length] = '\0';

            console_put_char(character);
        }
    }
}
