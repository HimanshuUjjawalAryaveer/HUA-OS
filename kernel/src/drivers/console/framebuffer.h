#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <limine.h>

void framebuffer_clear(
    struct limine_framebuffer *fb,
    uint8_t red,
    uint8_t green,
    uint8_t blue
);

void framebuffer_put_pixel(
    struct limine_framebuffer *fb,
    uint64_t x,
    uint64_t y,
    uint8_t red,
    uint8_t green,
    uint8_t blue
);

void framebuffer_fill_rect(
    struct limine_framebuffer *fb,
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint8_t red,
    uint8_t green,
    uint8_t blue
);

void framebuffer_shift_up(
    struct limine_framebuffer *fb,
    uint64_t pixel_rows,
    uint8_t red,
    uint8_t green,
    uint8_t blue
);

void framebuffer_shift_down(
    struct limine_framebuffer *fb,
    uint64_t pixel_rows,
    uint8_t red,
    uint8_t green,
    uint8_t blue
);

#endif
