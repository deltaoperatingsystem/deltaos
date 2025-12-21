#include <drivers/console.h>
#include <drivers/fb.h>
#include <boot/db.h>
#include <lib/font.h>
#include <kernel/device.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

static uint32 cursor_col = 0;
static uint32 cursor_row = 0;
static uint32 fg_color = 0xFFFFFF;  //white
static uint32 bg_color = 0x000000;  //black
static uint32 cols = 0;
static uint32 rows = 0;

//device ops
static ssize console_write(struct device *dev, const void *buf, size len) {
    (void)dev;
    const char *s = buf;
    for (size i = 0; i < len; i++) {
        con_putc(s[i]);
    }
    return len;
}

static struct device_ops console_ops = {
    .write = console_write
};

static struct device console_dev = {
    .name = "console",
    .type = DEV_CHAR,
    .subtype = SUBDEV_CONSOLE,
    .ops = &console_ops
};

void con_init(void) {
    if (!fb_available()) return;
    
    cols = fb_width() / FONT_WIDTH;
    rows = fb_height() / FONT_HEIGHT;
    cursor_col = 0;
    cursor_row = 0;
    
    //register with device manager
    device_register(&console_dev);
}

void con_clear(void) {
    fb_clear(bg_color);
    cursor_col = 0;
    cursor_row = 0;
}

void con_set_fg(uint32 color) {
    fg_color = color;
}

void con_set_bg(uint32 color) {
    bg_color = color;
}

static void draw_char(uint32 col, uint32 row, char c) {
    if (!fb_available() || col >= cols || row >= rows) return;
    
    uint32 x = col * FONT_WIDTH;
    uint32 y = row * FONT_HEIGHT;
    uint8 ch = (uint8)c;
    
    if (ch >= 128) ch = '?';  //unknown chars
    
    const uint8 *glyph = &font[ch * FONT_HEIGHT];
    
    for (uint32 py = 0; py < FONT_HEIGHT; py++) {
        uint8 row_bits = glyph[py];
        for (uint32 px = 0; px < FONT_WIDTH; px++) {
            uint32 color = (row_bits & (0x80 >> px)) ? fg_color : bg_color;
            fb_putpixel(x + px, y + py, color);
        }
    }
}

static void scroll(void) {
    fb_scroll(FONT_HEIGHT, bg_color);
}

static void newline(void) {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= rows) {
        scroll();
        cursor_row = rows - 1;
    }
}

void con_putc(char c) {
    if (c == '\n') {
        newline();
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\t') {
        cursor_col = (cursor_col + 4) & ~3;
        if (cursor_col >= cols) newline();
    } else if (c == '\b') {
        if (cursor_col == 0) { cursor_row--; cursor_col = cols; }
        else cursor_col--;
        draw_char(cursor_col, cursor_row, ' ');
    } else {
        draw_char(cursor_col, cursor_row, c);
        cursor_col++;
        if (cursor_col >= cols) newline();
    }
}

void con_print(const char *s) {
    while (*s) {
        con_putc(*s++);
    }
}

void con_print_at(uint32 col, uint32 row, const char *s) {
    cursor_col = col;
    cursor_row = row;
    con_print(s);
}

void con_set_cursor(uint32 col, uint32 row) {
    cursor_col = col;
    cursor_row = row;
}

uint32 con_cols(void) {
    return cols;
}

uint32 con_rows(void) {
    return rows;
}
