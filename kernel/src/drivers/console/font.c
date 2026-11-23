#include "drivers/console/font.h"
#include "drivers/console/framebuffer.h"

bool font_init(
    struct font *font,
    const struct limine_file *file
) {
    if (font == 0 || file == 0 ||
        file->address == 0 || file->size < 4) {
        return false;
    }

    const uint8_t *data = file->address;

    /* Validate PSF1 magic bytes. */
    if (data[0] != 0x36 || data[1] != 0x04) {
        return false;
    }

    uint8_t mode = data[2];
    uint8_t char_height = data[3];

    if (char_height == 0) {
        return false;
    }

    /* PSF1 mode bit 0 means 512 glyphs instead of 256. */
    uint16_t glyph_count = (mode & 0x01) ? 512 : 256;

    uint64_t glyph_data_size =
        (uint64_t)glyph_count * char_height;

    if (file->size < 4 + glyph_data_size) {
        return false;
    }

    font->glyphs = data + 4;
    font->glyph_count = glyph_count;
    font->char_height = char_height;

    return true;
}

void font_draw_char(
    struct limine_framebuffer *fb,
    const struct font *font,
    uint64_t x,
    uint64_t y,
    char character,
    uint32_t foreground,
    uint32_t background
) {
    if (fb == 0 || font == 0 ||
        font->glyphs == 0 || font->char_height == 0) {
        return;
    }

    uint8_t character_index = (uint8_t)character;

    if (character_index >= font->glyph_count) {
        character_index = '?';
    }

    const uint8_t *glyph =
        font->glyphs +
        (uint64_t)character_index * font->char_height;

    for (uint8_t row = 0; row < font->char_height; row++) {
        uint8_t bits = glyph[row];

        for (uint8_t col = 0; col < 8; col++) {
            bool pixel_set = (bits & (0x80 >> col)) != 0;

            uint32_t color = pixel_set ? foreground : background;

            uint8_t red = (color >> 16) & 0xFF;
            uint8_t green = (color >> 8) & 0xFF;
            uint8_t blue = color & 0xFF;

            framebuffer_put_pixel(
                fb, x + col, y + row,
                red, green, blue
            );
        }
    }
}

void font_draw_text_scaled(
    struct limine_framebuffer *fb,
    const struct font *font,
    uint64_t x,
    uint64_t y,
    const char *text,
    uint8_t scale,
    uint32_t foreground,
    uint32_t background
) {
    if (fb == 0 || font == 0 ||
        font->glyphs == 0 || text == 0 ||
        font->char_height == 0 || scale == 0) {
        return;
    }

    uint64_t cursor_x = x;

    while (*text != '\0') {

        uint8_t character_index = (uint8_t)*text++;

        if (character_index >= font->glyph_count) {
            character_index = '?';
        }

        const uint8_t *glyph =
            font->glyphs +
            (uint64_t)character_index * font->char_height;

        for (uint8_t row = 0; row < font->char_height; row++) {

            uint8_t bits = glyph[row];

            for (uint8_t col = 0; col < 8; col++) {

                bool pixel_set = (bits & (0x80 >> col)) != 0;

                uint32_t color =
                    pixel_set ? foreground : background;

                uint8_t red = (color >> 16) & 0xFF;
                uint8_t green = (color >> 8) & 0xFF;
                uint8_t blue = color & 0xFF;

                framebuffer_fill_rect(
                    fb,
                    cursor_x + ((uint64_t)col * scale),
                    y + ((uint64_t)row * scale),
                    scale,
                    scale,
                    red,
                    green,
                    blue
                );
            }
        }

        cursor_x += 8 * scale;
    }
}
