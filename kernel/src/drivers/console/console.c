#include "drivers/console/console.h"
#include "drivers/console/framebuffer.h"

#define FONT_WIDTH 8

#define MAX_COLUMNS 512
#define MAX_HISTORY_LINES 512

/*
 * One terminal cell stores:
 * - The character.
 * - Its foreground color.
 * - Its background color.
 */
struct console_cell {
    char character;
    uint32_t foreground;
    uint32_t background;
};

/*
 * Memory-backed terminal history.
 * Each row contains MAX_COLUMNS character cells.
 */
static struct console_cell
    console_history[MAX_HISTORY_LINES][MAX_COLUMNS];

/* Circular history-buffer management. */
static uint64_t history_start = 0;
static uint64_t history_count = 0;

/*
 * Number of rows the user has scrolled back
 * from the newest terminal output.
 *
 * 0 means the terminal follows the latest output.
 */
static uint64_t view_offset = 0;

/* Actual framebuffer dimensions in terminal cells. */
static uint64_t visible_columns = 0;
static uint64_t visible_rows = 0;

static struct limine_framebuffer *console_fb;
static const struct font *console_font;

static uint64_t cursor_x = 0;
static uint64_t cursor_y = 0;

static uint32_t foreground = 0xFFFFFF;
static uint32_t background = 0x101820;

bool console_init(
    struct limine_framebuffer *fb,
    const struct font *font
) {
    if (fb == 0 || font == 0 ||
        font->glyphs == 0 || font->char_height == 0 ||
        fb->width < FONT_WIDTH ||
        fb->height < font->char_height ||
        fb->bpp != 32) {
        return false;
    }

    /*
    * Calculate the actual terminal dimensions.
    */
    uint64_t columns = fb->width / FONT_WIDTH;
    uint64_t rows = fb->height / font->char_height;

    /*
    * Reject framebuffer dimensions that exceed
    * the capacity of our history buffer.
    */
    if (columns > MAX_COLUMNS ||
        rows > MAX_HISTORY_LINES) {
        return false;
    }

    visible_columns = columns;
    visible_rows = rows;

    console_fb = fb;
    console_font = font;

    cursor_x = 0;
    cursor_y = 0;

    console_clear();

    return true;
}

void console_set_color(
    uint32_t fg,
    uint32_t bg
) {
    foreground = fg;
    background = bg;
}

/*
 * Reset the terminal's stored text history.
 */
static void console_reset_history(void) {

    history_start = 0;
    history_count = 1;
    view_offset = 0;

    /*
     * Initialize the first logical row.
     */
    for (uint64_t column = 0;
         column < visible_columns;
         column++) {

        console_history[0][column].character = ' ';
        console_history[0][column].foreground = foreground;
        console_history[0][column].background = background;
    }
}

/*
 * Initialize one logical terminal row with blank cells.
 */
static void console_clear_history_line(uint64_t line) {

    for (uint64_t column = 0;
         column < visible_columns;
         column++) {

        console_history[line][column].character = ' ';
        console_history[line][column].foreground = foreground;
        console_history[line][column].background = background;
    }
}

/*
 * Return the index of the newest logical terminal row.
 */
static uint64_t console_current_history_line(void) {

    return (history_start + history_count - 1)
           % MAX_HISTORY_LINES;
}

/*
 * Find the first history row currently visible.
 */
static uint64_t console_first_visible_history_line(void) {

    if (history_count <= visible_rows) {
        return history_start;
    }

    uint64_t newest_line = console_current_history_line();

    uint64_t lines_back =
        visible_rows - 1 + view_offset;

    return
        (newest_line + MAX_HISTORY_LINES -
         (lines_back % MAX_HISTORY_LINES))
        % MAX_HISTORY_LINES;
}

/*
 * Draw one history row at a specified screen position.
 */
static void console_render_history_row(
    uint64_t screen_row,
    uint64_t history_line
) {

    if (screen_row >= visible_rows) {
        return;
    }

    for (uint64_t column = 0;
         column < visible_columns;
         column++) {

        struct console_cell *cell =
            &console_history[history_line][column];

        font_draw_char(
            console_fb,
            console_font,
            column * FONT_WIDTH,
            screen_row * console_font->char_height,
            cell->character,
            cell->foreground,
            cell->background
        );
    }
}

/*
 * Redraw the visible terminal using stored history.
 */
static void console_render_history(void) {

    if (console_fb == 0 || console_font == 0 ||
        history_count == 0) {
        return;
    }

    /*
     * Calculate the maximum available scrollback.
     */
    uint64_t max_offset = 0;

    if (history_count > visible_rows) {
        max_offset = history_count - visible_rows;
    }

    /*
     * Prevent scrolling beyond available history.
     */
    if (view_offset > max_offset) {
        view_offset = max_offset;
    }

    /*
     * Clear the framebuffer before redrawing.
     */
    framebuffer_clear(
        console_fb,
        (background >> 16) & 0xFF,
        (background >> 8) & 0xFF,
        background & 0xFF
    );

    uint64_t first_line;

    if (history_count <= visible_rows) {

        first_line = history_start;

    } else {

        uint64_t newest_line =
            console_current_history_line();

        uint64_t lines_back =
            visible_rows - 1 + view_offset;

        first_line =
            (newest_line + MAX_HISTORY_LINES -
             (lines_back % MAX_HISTORY_LINES))
            % MAX_HISTORY_LINES;
    }

    uint64_t lines_to_draw = history_count;

    if (lines_to_draw > visible_rows) {
        lines_to_draw = visible_rows;
    }

    /*
     * Draw every visible character cell.
     */
    for (uint64_t row = 0;
         row < lines_to_draw;
         row++) {

        uint64_t history_line =
            (first_line + row) % MAX_HISTORY_LINES;

        for (uint64_t column = 0;
             column < visible_columns;
             column++) {

            struct console_cell *cell =
                &console_history[history_line][column];

            font_draw_char(
                console_fb,
                console_font,
                column * FONT_WIDTH,
                row * console_font->char_height,
                cell->character,
                cell->foreground,
                cell->background
            );
        }
    }
}

/*
 * Create a new logical terminal row.
 */
static void console_history_newline(void) {

    uint64_t new_line;

    if (history_count < MAX_HISTORY_LINES) {

        new_line =
            (history_start + history_count)
            % MAX_HISTORY_LINES;

        history_count++;

    } else {

        /*
         * History is full.
         * Discard the oldest row and reuse its storage.
         */
        history_start =
            (history_start + 1) % MAX_HISTORY_LINES;

        new_line =
            (history_start + history_count - 1)
            % MAX_HISTORY_LINES;
    }

    console_clear_history_line(new_line);
}

void console_clear(void) {
    if (console_fb == 0) {
        return;
    }

    framebuffer_clear(
        console_fb,
        (background >> 16) & 0xFF,
        (background >> 8) & 0xFF,
        background & 0xFF
    );

    cursor_x = 0;
    cursor_y = 0;
    console_reset_history();
}

/*
 * Move the output cursor to the last visible terminal row.
 * Actual screen scrolling is handled by console_render_history().
 */
static void console_scroll(void) {

    cursor_y =
        console_fb->height - console_font->char_height;
}

static void console_newline(void) {

    cursor_x = 0;
    cursor_y += console_font->char_height;

    /*
     * Preserve the historical viewport as new rows arrive.
     */
    if (view_offset > 0) {
        view_offset++;
    }

    console_history_newline();

    /*
     * Check whether the cursor has passed the screen.
     */
    bool screen_must_scroll = false;

    if (cursor_y + console_font->char_height >
        console_fb->height) {

        console_scroll();
        screen_must_scroll = true;
    }

    /*
     * Redraw only when following the latest output
     * and the screen actually needs to scroll.
     */
    if (view_offset == 0 && screen_must_scroll) {

        /*
        * Move existing screen contents up by one text row.
        */
        framebuffer_shift_up(
            console_fb,
            console_font->char_height,
            (background >> 16) & 0xFF,
            (background >> 8) & 0xFF,
            background & 0xFF
        );

        /*
        * Draw only the newly exposed bottom history row.
        */
        console_render_history_row(
            visible_rows - 1,
            console_current_history_line()
        );
    }
}

void console_put_char(char character) {
    if (console_fb == 0 || console_font == 0) {
        return;
    }

    if (character == '\n') {
        console_newline();
        return;
    }

    if (character == '\r') {
        cursor_x = 0;
        return;
    }

    if (character == '\t') {
        for (int i = 0; i < 4; i++) {
            console_put_char(' ');
        }
        return;
    }

    if (cursor_x + FONT_WIDTH > console_fb->width) {
        console_newline();
    }

    if (cursor_y + console_font->char_height >
        console_fb->height) {
        console_scroll();
    }

    /*
    * Save the character and its colors in memory.
    */
    uint64_t history_line = console_current_history_line();
    uint64_t history_column = cursor_x / FONT_WIDTH;

    if (history_column < visible_columns) {

        console_history[history_line][history_column].character =
            character;

        console_history[history_line][history_column].foreground =
            foreground;

        console_history[history_line][history_column].background =
            background;
    }

    /*
    * Draw only the newly written character.
    *
    * Do not modify the framebuffer while the user
    * is viewing older terminal history.
    */
    if (view_offset == 0) {

        font_draw_char(
            console_fb,
            console_font,
            cursor_x,
            cursor_y,
            character,
            foreground,
            background
        );
    }

    /*
    * Advance the output cursor.
    */
    cursor_x += FONT_WIDTH;
}

void console_backspace(void) {
    if (console_fb == 0 || console_font == 0) {
        return;
    }

    // Do not move before the beginning of the current line.
    if (cursor_x < FONT_WIDTH) {
        return;
    }

    // Move the cursor one character cell backward.
    cursor_x -= FONT_WIDTH;

    /*
     * Remove the character from the stored history.
     */
    uint64_t history_line = console_current_history_line();
    uint64_t history_column = cursor_x / FONT_WIDTH;

    if (history_column < visible_columns) {

        console_history[history_line][history_column].character = ' ';

        console_history[history_line][history_column].foreground =
            foreground;

        console_history[history_line][history_column].background =
            background;
    }

    /*
    * Erase only the deleted character cell.
    */
    if (view_offset == 0) {

        font_draw_char(
            console_fb,
            console_font,
            cursor_x,
            cursor_y,
            ' ',
            foreground,
            background
        );
    }

}

void console_write(const char *text) {
    if (text == 0) {
        return;
    }

    while (*text != '\0') {
        console_put_char(*text);
        text++;
    }
}

/*
 * Scroll backward through terminal history.
 */
void console_scroll_up(uint64_t lines) {

    if (history_count <= visible_rows || lines == 0) {
        return;
    }

    uint64_t max_offset = history_count - visible_rows;

    if (view_offset >= max_offset) {
        return;
    }

    /*
     * One-line scrolling: shift existing pixels down
     * and draw only the newly exposed top row.
     */
    if (lines == 1) {

        view_offset++;

        framebuffer_shift_down(
            console_fb,
            console_font->char_height,
            (background >> 16) & 0xFF,
            (background >> 8) & 0xFF,
            background & 0xFF
        );

        console_render_history_row(
            0,
            console_first_visible_history_line()
        );

        return;
    }

    /*
     * Multi-line scrolling, such as Page Up.
     * Use the complete history renderer.
     */
    if (lines > max_offset - view_offset) {
        view_offset = max_offset;
    } else {
        view_offset += lines;
    }

    console_render_history();
}

/*
 * Scroll forward toward the latest terminal output.
 */
void console_scroll_down(uint64_t lines) {

    if (view_offset == 0 || lines == 0) {
        return;
    }

    /*
     * One-line scrolling: shift existing pixels up
     * and draw only the newly exposed bottom row.
     */
    if (lines == 1) {

        view_offset--;

        framebuffer_shift_up(
            console_fb,
            console_font->char_height,
            (background >> 16) & 0xFF,
            (background >> 8) & 0xFF,
            background & 0xFF
        );

        uint64_t first_line =
            console_first_visible_history_line();

        uint64_t bottom_line =
            (first_line + visible_rows - 1)
            % MAX_HISTORY_LINES;

        console_render_history_row(
            visible_rows - 1,
            bottom_line
        );

        return;
    }

    /*
     * Multi-line scrolling, such as Page Down.
     */
    if (lines > view_offset) {
        view_offset = 0;
    } else {
        view_offset -= lines;
    }

    console_render_history();
}

/*
 * Scroll upward by one complete visible screen.
 */
void console_page_up(void) {
    console_scroll_up(visible_rows);
}

/*
 * Scroll downward by one complete visible screen.
 */
void console_page_down(void) {
    console_scroll_down(visible_rows);
}
