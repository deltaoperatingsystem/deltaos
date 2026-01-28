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
int snprintf(char *buf, size n, const char *format, ...);
int vsnprintf(char *buf, size n, const char *format, __builtin_va_list args);
void debug_write(const char *buf, size count);
void set_outmode(enum output_mode m);
void io_enable_serial(void);

void kpanic(void *frame, const char *fmt, ...);

#endif