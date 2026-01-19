#ifndef __IO_H
#define __IO_H

#include <types.h>
#include <args.h>

//standard I/O handles (initialized by _io_init)
extern int32 __stdout;
extern int32 __stdin;

//initialize I/O
void _io_init(void);

//output functions
void puts(const char *str);
void putc(const char c);
void printf(const char *fmt, ...);
int snprintf(char *buf, size n, const char *format, ...);
int vsnprintf(char *buf, size n, const char *format, va_list args);

void dprintf(const char *fmt, ...);

#endif