#include <curses.h>
#include <io.h>
#include <system.h>
#include <args.h>
#include <mem.h>
#include <string.h>

typedef struct {
    //one screen cell and its colors
    char ch;
    uint32 fg;
    uint32 bg;
} curses_cell_t;

//VT handle and current screen state
static handle_t curses_vt = INVALID_HANDLE;
static bool curses_ready = false;
static uint32 curses_cols = 0;
static uint32 curses_rows = 0;
static uint32 curses_cursor_col = 0;
static uint32 curses_cursor_row = 0;
static bool curses_cursor_visible = true;
static uint32 curses_fg = CURSES_RGB(255, 255, 255);
static uint32 curses_bg = CURSES_RGB(0, 0, 0);
static curses_cell_t *curses_cells = NULL;
static curses_cell_t *curses_prev_cells = NULL;

static const char hex_digits[] = "0123456789ABCDEF";

static inline size curses_cell_count(void) {
    //total number of visible cells
    return (size)curses_cols * (size)curses_rows;
}

static inline size curses_cell_index(uint32 col, uint32 row) {
    //row major index into the cell arrays
    return (size)row * (size)curses_cols + col;
}

static bool curses_valid_pos(uint32 col, uint32 row) {
    //true if col,row is inside the current screen
    return curses_cells && col < curses_cols && row < curses_rows;
}

static void curses_store_cell(uint32 col, uint32 row, char ch, uint32 fg, uint32 bg) {
    if (!curses_valid_pos(col, row)) return;
    size idx = curses_cell_index(col, row);
    curses_cells[idx].ch = ch;
    curses_cells[idx].fg = fg;
    curses_cells[idx].bg = bg;
}

static void curses_fill_blank_row(uint32 row) {
    if (!curses_cells || row >= curses_rows) return;
    //blank row uses the current style
    for (uint32 col = 0; col < curses_cols; col++) {
        curses_store_cell(col, row, ' ', curses_fg, curses_bg);
    }
}

static void curses_fill_all(void) {
    if (!curses_cells) return;
    //clear the whole backing buffer
    for (uint32 row = 0; row < curses_rows; row++) {
        curses_fill_blank_row(row);
    }
}

static void curses_scroll(void) {
    if (!curses_cells || curses_rows == 0 || curses_cols == 0) return;

    //drop the first row and make room at the bottom
    size row_bytes = (size)curses_cols * sizeof(curses_cell_t);
    memmove(curses_cells, curses_cells + curses_cols, row_bytes * (curses_rows - 1));
    curses_fill_blank_row(curses_rows - 1);
}

static bool curses_ensure_capacity(char **buf, size *cap, size pos, size needed) {
    if (!buf || !*buf || !cap) return false;
    size size_max = (size)~(size)0;
    if (needed > size_max - pos) return false;

    size required = pos + needed;
    if (required <= *cap) return true;

    size new_cap = *cap ? *cap : 1;
    while (new_cap < required) {
        if (new_cap > size_max / 2) {
            new_cap = required + 1;
            break;
        }
        new_cap *= 2;
    }

    char *new_buf = realloc(*buf, new_cap);
    if (!new_buf) return false;
    *buf = new_buf;
    *cap = new_cap;
    return true;
}

static bool curses_emit_color_escape(char mode, uint32 color, char **buf, size *pos, size *cap) {
    if (!curses_ensure_capacity(buf, cap, *pos, 8)) return false;
    //escape plus 6 hex digits
    (*buf)[(*pos)++] = (char)27;
    (*buf)[(*pos)++] = mode;
    for (int shift = 20; shift >= 0; shift -= 4) {
        (*buf)[(*pos)++] = hex_digits[(color >> shift) & 0xF];
    }
    return true;
}

static bool curses_emit_cursor_escape(uint32 row, uint32 col, char **buf, size *pos, size *cap) {
    if (!curses_ensure_capacity(buf, cap, *pos, 10)) return false;
    //escape plus row and col in hex
    (*buf)[(*pos)++] = (char)27;
    (*buf)[(*pos)++] = 'P';
    for (int shift = 12; shift >= 0; shift -= 4) {
        (*buf)[(*pos)++] = hex_digits[(row >> shift) & 0xF];
    }
    for (int shift = 12; shift >= 0; shift -= 4) {
        (*buf)[(*pos)++] = hex_digits[(col >> shift) & 0xF];
    }
    return true;
}

static bool curses_emit_cursor_visibility_escape(bool visible, char **buf, size *pos, size *cap) {
    if (!curses_ensure_capacity(buf, cap, *pos, 3)) return false;
    //tell vt whether to show the cursor
    (*buf)[(*pos)++] = (char)27;
    (*buf)[(*pos)++] = 'v';
    (*buf)[(*pos)++] = visible ? '1' : '0';
    return true;
}

static void curses_write_terminal_reset(bool show_cursor, bool move_cursor) {
    if (curses_vt == INVALID_HANDLE) return;

    char buf[29];
    size pos = 0;

    buf[pos++] = (char)27;
    buf[pos++] = 'f';
    for (int shift = 20; shift >= 0; shift -= 4) {
        buf[pos++] = hex_digits[(CURSES_RGB(255, 255, 255) >> shift) & 0xF];
    }

    buf[pos++] = (char)27;
    buf[pos++] = 'b';
    for (int shift = 20; shift >= 0; shift -= 4) {
        buf[pos++] = hex_digits[(CURSES_RGB(0, 0, 0) >> shift) & 0xF];
    }

    if (show_cursor) {
        buf[pos++] = (char)27;
        buf[pos++] = 'v';
        buf[pos++] = '1';
    }

    if (move_cursor && curses_cursor_col < curses_cols && curses_cursor_row < curses_rows) {
        buf[pos++] = (char)27;
        buf[pos++] = 'P';
        for (int shift = 12; shift >= 0; shift -= 4) {
            buf[pos++] = hex_digits[(curses_cursor_row >> shift) & 0xF];
        }
        for (int shift = 12; shift >= 0; shift -= 4) {
            buf[pos++] = hex_digits[(curses_cursor_col >> shift) & 0xF];
        }
    }

    handle_write(curses_vt, buf, (int)pos);
}

static int curses_open_vt(void) {
    if (curses_vt != INVALID_HANDLE) return 0;
    curses_vt = get_obj(INVALID_HANDLE, "$devices/vt0", RIGHT_WRITE | RIGHT_GET_INFO);
    if (curses_vt == INVALID_HANDLE) return -1;
    return 0;
}

static int curses_query_size(void) {
    vt_info_t info;
    if (object_get_info(curses_vt, OBJ_INFO_VT_STATE, &info, sizeof(info)) < 0) return -1;
    if (info.cols == 0 || info.rows == 0) return -1;

    //cache the current terminal size and reset the cursor
    curses_cols = info.cols;
    curses_rows = info.rows;
    curses_cursor_col = 0;
    curses_cursor_row = 0;
    curses_cursor_visible = true;
    return 0;
}

static bool curses_render_cell(char **buf, size *pos, size *cap, uint32 *fg, uint32 *bg, const curses_cell_t *cell) {
    //emit style changes before the character itself
    if (cell->fg != *fg) {
        if (!curses_emit_color_escape('f', cell->fg, buf, pos, cap)) return false;
        *fg = cell->fg;
    }
    if (cell->bg != *bg) {
        if (!curses_emit_color_escape('b', cell->bg, buf, pos, cap)) return false;
        *bg = cell->bg;
    }
    if (!curses_ensure_capacity(buf, cap, *pos, 1)) return false;
    (*buf)[(*pos)++] = cell->ch;
    return true;
}

int curses_init(void) {
    if (curses_ready) return 0;
    if (curses_open_vt() < 0) return -1;
    if (curses_query_size() < 0) {
        handle_close(curses_vt);
        curses_vt = INVALID_HANDLE;
        return -1;
    }

    //keep a current buffer and a previous buffer so flush can diff them
    curses_cells = calloc(curses_cell_count(), sizeof(curses_cell_t));
    curses_prev_cells = calloc(curses_cell_count(), sizeof(curses_cell_t));
    if (!curses_cells || !curses_prev_cells) {
        handle_close(curses_vt);
        curses_vt = INVALID_HANDLE;
        free(curses_cells);
        curses_cells = NULL;
        free(curses_prev_cells);
        curses_prev_cells = NULL;
        return -1;
    }

    kbd_flush();
    curses_fill_all();
    curses_ready = true;
    //force a full redraw on first flush
    curses_clear();
    return 0;
}

void curses_shutdown(void) {
    if (curses_ready) {
        curses_restore_terminal();
    }
    if (curses_vt != INVALID_HANDLE) {
        handle_write(curses_vt, "\f", 1);
        handle_close(curses_vt);
        curses_vt = INVALID_HANDLE;
    }
    if (curses_cells) {
        free(curses_cells);
        curses_cells = NULL;
    }
    if (curses_prev_cells) {
        free(curses_prev_cells);
        curses_prev_cells = NULL;
    }
    curses_ready = false;
}

void curses_restore_terminal(void) {
    curses_reset_style();
    curses_cursor_visible = true;
    curses_write_terminal_reset(true, true);
}

void curses_reset_style(void) {
    //default colors
    curses_fg = CURSES_RGB(255, 255, 255);
    curses_bg = CURSES_RGB(0, 0, 0);
}

void curses_set_fg(uint32 color) {
    //mask to rgb24
    curses_fg = color & 0xFFFFFFu;
}

void curses_set_bg(uint32 color) {
    //mask to rgb24
    curses_bg = color & 0xFFFFFFu;
}

void curses_show_cursor(bool visible) {
    //cursor is only hidden at flush time
    curses_cursor_visible = visible;
}

void curses_set_cursor(uint32 col, uint32 row) {
    //clamp by ignoring out of range values
    if (col < curses_cols) curses_cursor_col = col;
    if (row < curses_rows) curses_cursor_row = row;
}

uint32 curses_get_cursor_col(void) {
    return curses_cursor_col;
}

uint32 curses_get_cursor_row(void) {
    return curses_cursor_row;
}

uint32 curses_get_cols(void) {
    return curses_cols;
}

uint32 curses_get_rows(void) {
    return curses_rows;
}

void curses_clear(void) {
    if (!curses_ready) {
        if (curses_init() < 0) return;
    }
    //clear the buffer and move the cursor home
    curses_fill_all();
    curses_cursor_col = 0;
    curses_cursor_row = 0;
}

void curses_fill_rect(uint32 col, uint32 row, uint32 width, uint32 height, char ch) {
    if (!curses_ready && curses_init() < 0) return;
    if (!curses_cells || width == 0 || height == 0) return;
    if (col >= curses_cols || row >= curses_rows) return;

    //clip the rectangle to the visible screen
    uint32 end_col = col + width;
    uint32 end_row = row + height;
    if (end_col > curses_cols || end_col < col) end_col = curses_cols;
    if (end_row > curses_rows || end_row < row) end_row = curses_rows;

    for (uint32 y = row; y < end_row; y++) {
        for (uint32 x = col; x < end_col; x++) {
            curses_store_cell(x, y, ch, curses_fg, curses_bg);
        }
    }
}

void curses_clear_rect(uint32 col, uint32 row, uint32 width, uint32 height) {
    //clear just means fill with spaces
    curses_fill_rect(col, row, width, height, ' ');
}

static void curses_write_cell(char c) {
    if (!curses_cells || curses_cols == 0 || curses_rows == 0) return;

    if (c == '\n') {
        //newline wraps to the next row
        curses_cursor_col = 0;
        if (curses_cursor_row + 1 >= curses_rows) {
            curses_scroll();
        } else {
            curses_cursor_row++;
        }
        return;
    }

    if (c == '\r') {
        //carriage return just moves to column 0
        curses_cursor_col = 0;
        return;
    }

    if (c == '\t') {
        //tabs always snap to 4 columns
        uint32 spaces = 4 - (curses_cursor_col & 3);
        for (uint32 i = 0; i < spaces; i++) {
            curses_write_cell(' ');
        }
        return;
    }

    if (c == '\b') {
        //backspace only erases inside the current row
        if (curses_cursor_col > 0) {
            curses_cursor_col--;
            curses_store_cell(curses_cursor_col, curses_cursor_row, ' ', curses_fg, curses_bg);
        }
        return;
    }

    //normal printable char
    curses_store_cell(curses_cursor_col, curses_cursor_row, c, curses_fg, curses_bg);

    curses_cursor_col++;
    if (curses_cursor_col >= curses_cols) {
        curses_cursor_col = 0;
        if (curses_cursor_row + 1 >= curses_rows) {
            curses_scroll();
        } else {
            curses_cursor_row++;
        }
    }
}

static void curses_write_raw(const char *s) {
    if (!s) return;
    //feed each byte through the cursor logic
    while (*s) curses_write_cell(*s++);
}

void curses_putc(char c) {
    if (!curses_ready && curses_init() < 0) return;
    curses_write_cell(c);
}

void curses_putc_at(uint32 col, uint32 row, char c) {
    if (!curses_ready && curses_init() < 0) return;
    if (!curses_valid_pos(col, row)) return;
    curses_store_cell(col, row, c, curses_fg, curses_bg);
    curses_cursor_col = col;
    curses_cursor_row = row;
    if (curses_cursor_col + 1 < curses_cols) {
        curses_cursor_col++;
    }
}

void curses_write(const char *s) {
    if (!s) return;
    if (!curses_ready && curses_init() < 0) return;
    curses_write_raw(s);
    curses_flush();
}

void curses_write_at(uint32 col, uint32 row, const char *s, uint32 max_width) {
    if (!s) return;
    if (!curses_ready && curses_init() < 0) return;
    if (col >= curses_cols || row >= curses_rows) return;

    curses_set_cursor(col, row);
    //limit width so callers can draw inside fixed regions
    uint32 remaining = curses_cols - col;
    if (max_width > 0 && max_width < remaining) remaining = max_width;

    while (*s && remaining > 0) {
        curses_write_cell(*s++);
        remaining--;
    }
}

void curses_writeln(const char *s) {
    if (!curses_ready && curses_init() < 0) return;
    if (s) {
        curses_write_raw(s);
    }
    //append a newline and flush
    curses_write_cell('\n');
    curses_flush();
}

void curses_puts_at(uint32 col, uint32 row, const char *s) {
    if (!s) return;
    curses_set_cursor(col, row);
    curses_write_raw(s);
}

void curses_hline(uint32 col, uint32 row, uint32 width, char ch) {
    //draw a one row rectangle
    curses_fill_rect(col, row, width, 1, ch);
}

void curses_vline(uint32 col, uint32 row, uint32 height, char ch) {
    //draw a one column rectangle
    curses_fill_rect(col, row, 1, height, ch);
}

void curses_draw_box(uint32 col, uint32 row, uint32 width, uint32 height) {
    if (!curses_ready && curses_init() < 0) return;
    if (width < 2 || height < 2) return;
    if (col >= curses_cols || row >= curses_rows) return;
    if (width > curses_cols - col) width = curses_cols - col;
    if (height > curses_rows - row) height = curses_rows - row;
    if (width < 2 || height < 2) return;

    //simple ascii box
    curses_hline(col + 1, row, width - 2, '-');
    curses_hline(col + 1, row + height - 1, width - 2, '-');
    curses_vline(col, row + 1, height - 2, '|');
    curses_vline(col + width - 1, row + 1, height - 2, '|');
    curses_putc_at(col, row, '+');
    curses_putc_at(col + width - 1, row, '+');
    curses_putc_at(col, row + height - 1, '+');
    curses_putc_at(col + width - 1, row + height - 1, '+');
}

void curses_center_text(uint32 row, const char *s) {
    if (!s) return;
    if (!curses_ready && curses_init() < 0) return;
    if (row >= curses_rows) return;

    //center by character count, not glyph width
    uint32 len = (uint32)strlen(s);
    if (len >= curses_cols) {
        curses_write_at(0, row, s, curses_cols);
        return;
    }

    uint32 col = (curses_cols - len) / 2;
    curses_write_at(col, row, s, len);
}

void curses_flush(void) {
    if (!curses_ready || !curses_cells || curses_vt == INVALID_HANDLE) return;

    //build one vt update string for all changed cells
    size cap = 4096;
    char *buf = malloc(cap);
    if (!buf) return;

    size pos = 0;
    uint32 fg = CURSES_RGB(255, 255, 255);
    uint32 bg = CURSES_RGB(0, 0, 0);

    for (uint32 row = 0; row < curses_rows; row++) {
        uint32 col = 0;
        while (col < curses_cols) {
            size idx = (size)row * (size)curses_cols + col;
            const curses_cell_t *cell = &curses_cells[idx];
            const curses_cell_t *prev = &curses_prev_cells[idx];

            if (cell->ch == prev->ch && cell->fg == prev->fg && cell->bg == prev->bg) {
                col++;
                continue;
            }

            //jump to the first changed cell in this run
            if (!curses_emit_cursor_escape(row, col, &buf, &pos, &cap)) {
                free(buf);
                return;
            }

            while (col < curses_cols) {
                idx = (size)row * (size)curses_cols + col;
                cell = &curses_cells[idx];
                prev = &curses_prev_cells[idx];
                if (cell->ch == prev->ch && cell->fg == prev->fg && cell->bg == prev->bg) {
                    break;
                }

                if (!curses_render_cell(&buf, &pos, &cap, &fg, &bg, cell)) {
                    free(buf);
                    return;
                }
                col++;
            }
        }
    }

    if (!curses_emit_cursor_visibility_escape(curses_cursor_visible, &buf, &pos, &cap)) {
        free(buf);
        return;
    }

    if (curses_cursor_visible && curses_cursor_col < curses_cols && curses_cursor_row < curses_rows) {
        //put the cursor back where the caller left it
        if (!curses_emit_cursor_escape(curses_cursor_row, curses_cursor_col, &buf, &pos, &cap)) {
            free(buf);
            return;
        }
    }

    if (pos > 0) {
        if (handle_write(curses_vt, buf, (int)pos) == (ssize)pos) {
            memcpy(curses_prev_cells, curses_cells, curses_cell_count() * sizeof(curses_cell_t));
        }
    }

    free(buf);
}

void curses_printf(const char *fmt, ...) {
    if (!fmt) return;
    if (!curses_ready && curses_init() < 0) return;

    //small fixed buffer, same as the rest of this codebase
    va_list args;
    va_start(args, fmt);
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        size to_write = (len < (int)sizeof(buf)) ? (size)len : (sizeof(buf) - 1);
        for (size i = 0; i < to_write; i++) curses_write_cell(buf[i]);
        curses_flush();
    }
}

int curses_read(kbd_event_t *event) {
    if (!event) return -1;
    if (!curses_ready && curses_init() < 0) return -1;
    //blocking read straight from keyboard
    return kbd_read(event);
}

int curses_try_read(kbd_event_t *event) {
    if (!event) return -1;
    if (!curses_ready && curses_init() < 0) return -1;
    //nonblocking read
    return kbd_try_read(event);
}

char curses_getchar(void) {
    if (!curses_ready && curses_init() < 0) return 0;
    //blocking char helper
    return kbd_getchar();
}

bool curses_event_is_submit(const kbd_event_t *event) {
    return event && event->pressed && event->codepoint == '\n';
}

bool curses_event_is_backspace(const kbd_event_t *event) {
    return event && event->pressed && event->codepoint == '\b';
}

bool curses_event_is_printable(const kbd_event_t *event) {
    //plain ascii printable range
    return event && event->pressed && event->codepoint >= 32 && event->codepoint < 127;
}

char curses_event_char(const kbd_event_t *event) {
    //return 0 if the event is not printable
    if (!curses_event_is_printable(event)) return 0;
    return (char)event->codepoint;
}
