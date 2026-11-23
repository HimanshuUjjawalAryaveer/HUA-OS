#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

struct font {
    const uint8_t *glyphs;
    uint16_t glyph_count;
    uint8_t char_height;
};

bool font_init(
    struct font *font,
    const struct limine_file *file
);

void font_draw_char(
    struct limine_framebuffer *fb,
    const struct font *font,
    uint64_t x,
    uint64_t y,
    char character,
    uint32_t foreground,
    uint32_t background
);

void font_draw_text_scaled(
    struct limine_framebuffer *fb,
    const struct font *font,
    uint64_t x,
    uint64_t y,
    const char *text,
    uint8_t scale,
    uint32_t foreground,
    uint32_t background
);

#endif
