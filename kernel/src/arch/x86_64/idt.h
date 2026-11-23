#ifndef HUA_IDT_H
#define HUA_IDT_H

#include <stdbool.h>

#define IDT_ENTRY_COUNT 256

// Install HUA's Interrupt Descriptor Table.
bool idt_init(void);

// Run basic IDT entry and return tests.
bool idt_run_tests(void);

#endif
