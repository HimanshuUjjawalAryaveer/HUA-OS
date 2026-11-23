#include "drivers/console/framebuffer.h"

static uint32_t fb_channel(
    uint8_t value,
    uint8_t mask_size,
    uint8_t mask_shift
) {
    uint64_t max = ((uint64_t)1 << mask_size) - 1;

    return (uint32_t)((value * max / 255) << mask_shift);
}

static uint32_t fb_pixel(
    struct limine_framebuffer *fb,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    return fb_channel(red, fb->red_mask_size, fb->red_mask_shift)
         | fb_channel(green, fb->green_mask_size, fb->green_mask_shift)
         | fb_channel(blue, fb->blue_mask_size, fb->blue_mask_shift);
}

void framebuffer_put_pixel(
    struct limine_framebuffer *fb,
    uint64_t x,
    uint64_t y,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    if (fb == 0 || fb->address == 0 ||
        fb->bpp != 32 ||
        fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        return;
    }

    if (x >= fb->width || y >= fb->height) {
        return;
    }

    volatile uint32_t *pixels = fb->address;

    uint64_t pitch_pixels = fb->pitch / 4;

    pixels[y * pitch_pixels + x] = fb_pixel(fb, red, green, blue);
}

void framebuffer_fill_rect(
    struct limine_framebuffer *fb,
    uint64_t x,
    uint64_t y,
    uint64_t width,
    uint64_t height,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    if (fb == 0 || fb->address == 0 ||
        fb->bpp != 32 ||
        fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        return;
    }

    if (x >= fb->width || y >= fb->height) {
        return;
    }

    if (width > fb->width - x) {
        width = fb->width - x;
    }

    if (height > fb->height - y) {
        height = fb->height - y;
    }

    volatile uint32_t *pixels = fb->address;
    uint64_t pitch_pixels = fb->pitch / 4;

    uint32_t color = fb_pixel(fb, red, green, blue);

    for (uint64_t row = 0; row < height; row++) {
        for (uint64_t col = 0; col < width; col++) {
            pixels[(y + row) * pitch_pixels + (x + col)] = color;
        }
    }
}

void framebuffer_clear(
    struct limine_framebuffer *fb,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    if (fb == 0) {
        return;
    }

    framebuffer_fill_rect(
        fb, 0, 0, fb->width, fb->height,
        red, green, blue
    );
}

/*
 * Move framebuffer contents upward.
 * The newly exposed bottom area is filled with the
 * requested background color.
 */
void framebuffer_shift_up(
    struct limine_framebuffer *fb,
    uint64_t pixel_rows,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    if (fb == 0 || fb->address == 0 ||
        fb->bpp != 32 ||
        fb->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        pixel_rows == 0) {
        return;
    }

    if (pixel_rows > fb->height) {
        pixel_rows = fb->height;
    }

    volatile uint32_t *pixels = fb->address;
    uint64_t pitch_pixels = fb->pitch / 4;

    /*
     * Copy from top to bottom.
     * This direction is safe because the destination
     * is always above the source.
     */
    for (uint64_t y = 0; y < fb->height - pixel_rows; y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            pixels[y * pitch_pixels + x] =
                pixels[(y + pixel_rows) * pitch_pixels + x];
        }
    }

    uint32_t color = fb_pixel(fb, red, green, blue);

    /*
     * Clear the newly exposed bottom area.
     */
    for (uint64_t y = fb->height - pixel_rows;
         y < fb->height;
         y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            pixels[y * pitch_pixels + x] = color;
        }
    }
}

/*
 * Move framebuffer contents downward.
 * The newly exposed top area is filled with the
 * requested background color.
 */
void framebuffer_shift_down(
    struct limine_framebuffer *fb,
    uint64_t pixel_rows,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    if (fb == 0 || fb->address == 0 ||
        fb->bpp != 32 ||
        fb->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        pixel_rows == 0) {
        return;
    }

    if (pixel_rows > fb->height) {
        pixel_rows = fb->height;
    }

    volatile uint32_t *pixels = fb->address;
    uint64_t pitch_pixels = fb->pitch / 4;

    /*
     * Copy from bottom to top.
     * This direction prevents overwriting source pixels.
     */
    for (uint64_t y = fb->height; y > pixel_rows; y--) {
        for (uint64_t x = 0; x < fb->width; x++) {
            pixels[(y - 1) * pitch_pixels + x] =
                pixels[(y - 1 - pixel_rows) * pitch_pixels + x];
        }
    }

    uint32_t color = fb_pixel(fb, red, green, blue);

    /*
     * Clear the newly exposed top area.
     */
    for (uint64_t y = 0; y < pixel_rows; y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            pixels[y * pitch_pixels + x] = color;
        }
    }
}
