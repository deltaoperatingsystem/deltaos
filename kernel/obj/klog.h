#ifndef KLOG_H
#define KLOG_H

#include <arch/types.h>

//initialize klog and register in namespace
void klog_init(void);

//append a single character to the kernel log
void klog_putc(char c);

//append a string of specified length to the kernel log
void klog_write(const char *s, size len);

#endif
