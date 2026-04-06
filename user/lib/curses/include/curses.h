#ifndef CURSES_H
#define CURSES_H

#include <types.h>
#include <keyboard.h>

#define CURSES_RGB(r, g, b) ((uint32)(((r) << 16) | ((g) << 8) | (b)))

int curses_init(void);
void curses_shutdown(void);

void curses_clear(void);
void curses_clear_rect(uint32 col, uint32 row, uint32 width, uint32 height);
void curses_fill_rect(uint32 col, uint32 row, uint32 width, uint32 height, char ch);
void curses_reset_style(void);
void curses_set_fg(uint32 color);
void curses_set_bg(uint32 color);
void curses_show_cursor(bool visible);
void curses_set_cursor(uint32 col, uint32 row);
uint32 curses_get_cursor_col(void);
uint32 curses_get_cursor_row(void);
uint32 curses_get_cols(void);
uint32 curses_get_rows(void);
void curses_flush(void);

void curses_putc(char c);
void curses_putc_at(uint32 col, uint32 row, char c);
void curses_write(const char *s);
void curses_write_at(uint32 col, uint32 row, const char *s, uint32 max_width);
void curses_writeln(const char *s);
void curses_printf(const char *fmt, ...);
void curses_puts_at(uint32 col, uint32 row, const char *s);
void curses_hline(uint32 col, uint32 row, uint32 width, char ch);
void curses_vline(uint32 col, uint32 row, uint32 height, char ch);
void curses_draw_box(uint32 col, uint32 row, uint32 width, uint32 height);
void curses_center_text(uint32 row, const char *s);

int curses_read(kbd_event_t *event);
int curses_try_read(kbd_event_t *event);
char curses_getchar(void);
bool curses_event_is_submit(const kbd_event_t *event);
bool curses_event_is_backspace(const kbd_event_t *event);
bool curses_event_is_printable(const kbd_event_t *event);
char curses_event_char(const kbd_event_t *event);

#endif
