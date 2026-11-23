
#ifndef HUA_X86_IO_H
#define HUA_X86_IO_H

#include <stdint.h>

/*
 * x86 port I/O helpers.
 *
 * These instructions access the processor's I/O port space,
 * not memory-mapped device registers.
 */

static inline void x86_outb(uint16_t port, uint8_t value) {
    asm volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
        : "memory"
    );
}

static inline uint8_t x86_inb(uint16_t port) {
    uint8_t value;

    asm volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
        : "memory"
    );

    return value;
}

static inline void x86_outw(uint16_t port, uint16_t value) {
    asm volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
        : "memory"
    );
}

static inline uint16_t x86_inw(uint16_t port) {
    uint16_t value;

    asm volatile (
        "inw %1, %0"
        : "=a"(value)
        : "Nd"(port)
        : "memory"
    );

    return value;
}

/*
 * Traditional short I/O delay.
 */
static inline void x86_io_wait(void) {
    x86_outb(0x80, 0);
}

#endif
