#include "interrupts/irq.h"

#include <stddef.h>

// -----------------------------------------------------------------------------
// IRQ descriptor
// -----------------------------------------------------------------------------

struct irq_descriptor {
    irq_handler_t handler;
    void *context;
    uint64_t interrupt_count;
    bool registered;
};

// One descriptor for every possible interrupt vector.
static struct irq_descriptor irq_descriptors[IRQ_VECTOR_COUNT];

// -----------------------------------------------------------------------------
// Interrupt state protection
// -----------------------------------------------------------------------------

// Save the current CPU flags and disable maskable interrupts.
static uint64_t irq_save_disable(void) {

    uint64_t flags;

    asm volatile (
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

// Restore the previous interrupt-enable state.
static void irq_restore(uint64_t flags) {

    if (flags & (1ULL << 9)) {
        asm volatile ("sti" ::: "memory");
    } else {
        asm volatile ("cli" ::: "memory");
    }
}

// -----------------------------------------------------------------------------
// Handler registration
// -----------------------------------------------------------------------------

bool irq_register_handler(
    uint8_t vector,
    irq_handler_t handler,
    void *context
) {

    if (vector < IRQ_VECTOR_FIRST ||
        vector > IRQ_VECTOR_LAST ||
        handler == NULL) {
        return false;
    }

    uint64_t flags = irq_save_disable();

    struct irq_descriptor *descriptor =
        &irq_descriptors[vector];

    if (descriptor->registered) {
        irq_restore(flags);
        return false;
    }

    descriptor->handler = handler;
    descriptor->context = context;
    descriptor->interrupt_count = 0;
    descriptor->registered = true;

    irq_restore(flags);

    return true;
}

// -----------------------------------------------------------------------------
// Handler unregistration
// -----------------------------------------------------------------------------

bool irq_unregister_handler(uint8_t vector) {

    if (vector < IRQ_VECTOR_FIRST ||
        vector > IRQ_VECTOR_LAST) {
        return false;
    }

    uint64_t flags = irq_save_disable();

    struct irq_descriptor *descriptor =
        &irq_descriptors[vector];

    if (!descriptor->registered) {
        irq_restore(flags);
        return false;
    }

    descriptor->handler = NULL;
    descriptor->context = NULL;
    descriptor->registered = false;

    irq_restore(flags);

    return true;
}

// -----------------------------------------------------------------------------
// Interrupt dispatch
// -----------------------------------------------------------------------------

bool irq_dispatch(struct interrupt_frame *frame) {

    if (frame == NULL) {
        return false;
    }

    if (frame->vector < IRQ_VECTOR_FIRST ||
        frame->vector > IRQ_VECTOR_LAST) {
        return false;
    }

    struct irq_descriptor *descriptor =
        &irq_descriptors[frame->vector];

    if (!descriptor->registered ||
        descriptor->handler == NULL) {
        return false;
    }

    descriptor->interrupt_count++;

    descriptor->handler(
        frame,
        descriptor->context
    );

    return true;
}

// -----------------------------------------------------------------------------
// Interrupt statistics
// -----------------------------------------------------------------------------

bool irq_get_stats(
    uint8_t vector,
    struct irq_stats *stats
) {

    if (stats == NULL ||
        vector < IRQ_VECTOR_FIRST ||
        vector > IRQ_VECTOR_LAST) {
        return false;
    }

    uint64_t flags = irq_save_disable();

    struct irq_descriptor *descriptor =
        &irq_descriptors[vector];

    stats->registered = descriptor->registered;
    stats->interrupt_count = descriptor->interrupt_count;

    irq_restore(flags);

    return true;
}

// -----------------------------------------------------------------------------
// IRQ self-tests
// -----------------------------------------------------------------------------

static volatile uint64_t irq_test_handler_count = 0;

static void irq_test_handler(
    struct interrupt_frame *frame,
    void *context
) {

    (void)context;

    if (frame != NULL && frame->vector == 0xF0) {
        irq_test_handler_count++;
    }
}

bool irq_run_tests(void) {

    const uint8_t test_vector = 0xF0;

    irq_test_handler_count = 0;

    // Test 1: Register a handler.
    if (!irq_register_handler(
            test_vector,
            irq_test_handler,
            NULL)) {
        return false;
    }

    // Test 2: Reject duplicate registration.
    if (irq_register_handler(
            test_vector,
            irq_test_handler,
            NULL)) {

        irq_unregister_handler(test_vector);
        return false;
    }

    // Test 3: Deliver a software interrupt.
    //
    // Vector 0xF0 is configured in the IDT and has its own
    // assembly stub. This tests the complete entry-to-dispatch path.
    asm volatile ("int $0xF0" ::: "memory");

    // Test 4: Verify that the handler executed.
    struct irq_stats stats;

    if (!irq_get_stats(test_vector, &stats)) {
        irq_unregister_handler(test_vector);
        return false;
    }

    if (!stats.registered ||
        stats.interrupt_count != 1 ||
        irq_test_handler_count != 1) {

        irq_unregister_handler(test_vector);
        return false;
    }

    // Test 5: Unregister the handler.
    if (!irq_unregister_handler(test_vector)) {
        return false;
    }

    // Test 6: Verify that it is no longer registered.
    if (!irq_get_stats(test_vector, &stats)) {
        return false;
    }

    if (stats.registered) {
        return false;
    }

    return true;
}
