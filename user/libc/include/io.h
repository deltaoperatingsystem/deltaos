#ifndef __IO_H
#define __IO_H

#include <types.h>

//standard I/O handles (initialized by _io_init)
extern int32 __stdout;
extern int32 __stdin;

//initialize I/O
void _io_init(void);

//output functions
void puts(const char *str);
void putc(const char c);
void printf(const char *fmt, ...);

#endif