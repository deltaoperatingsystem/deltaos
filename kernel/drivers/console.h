#ifndef DRIVERS_CONSOLE_H
#define DRIVERS_CONSOLE_H

#include <arch/types.h>

//initialize console (requires fb_init first)
void con_init(void);

//clear screen
void con_clear(void);

//flush backbuffer to screen (for double buffering)
void con_flush(void);

//set text colors
void con_set_fg(uint32 color);
void con_set_bg(uint32 color);

//print string at cursor, advances cursor
void con_print(const char *s);

//print at specific position (row, col)
void con_print_at(uint32 col, uint32 row, const char *s);

//put single character at cursor
void con_putc(char c);

//set cursor position
void con_set_cursor(uint32 col, uint32 row);

//get console dimensions in characters
uint32 con_cols(void);
uint32 con_rows(void);

#endif
