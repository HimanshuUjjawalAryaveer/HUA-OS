#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/console/font.h"

bool console_init(
    struct limine_framebuffer *fb,
    const struct font *font
);

void console_set_color(
    uint32_t foreground,
    uint32_t background
);

void console_clear(void);

void console_write(const char *text);

void console_put_char(char character);

void console_backspace(void);

void console_scroll_up(uint64_t lines);

void console_scroll_down(uint64_t lines);

void console_page_up(void);

void console_page_down(void);

#endif
