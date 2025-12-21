#ifndef LIB_IO_H
#define LIB_IO_H

#include <arch/types.h>
#include <string.h>

enum output_mode {
    SERIAL,
    CONSOLE,
};

void putc(const char c);
void puts(const char *s);
void printf(const char *format, ...);
void set_outmode(enum output_mode m);

#endif