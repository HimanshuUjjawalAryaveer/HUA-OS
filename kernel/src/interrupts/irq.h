#ifndef HUA_IRQ_H
#define HUA_IRQ_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/x86_64/interrupt_frame.h"

#define IRQ_VECTOR_FIRST 32
#define IRQ_VECTOR_LAST  254
#define IRQ_VECTOR_COUNT 256

typedef void (*irq_handler_t)(
    struct interrupt_frame *frame,
    void *context
);

struct irq_stats {
    bool registered;
    uint64_t interrupt_count;
};

// Register a handler for an interrupt vector.
bool irq_register_handler(
    uint8_t vector,
    irq_handler_t handler,
    void *context
);

// Remove a registered handler.
bool irq_unregister_handler(uint8_t vector);

// Dispatch an interrupt to its registered handler.
bool irq_dispatch(struct interrupt_frame *frame);

// Retrieve statistics for an interrupt vector.
bool irq_get_stats(
    uint8_t vector,
    struct irq_stats *stats
);

// Run generic IRQ core self-tests.
bool irq_run_tests(void);

#endif
