#ifndef CURSOR_H
#define CURSOR_H

#include <types.h>

int cursor_get_width(void);
int cursor_get_height(void);
uint32 cursor_get_pixel(int x, int y);

#endif
