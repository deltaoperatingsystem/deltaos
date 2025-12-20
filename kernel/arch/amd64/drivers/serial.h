#ifndef _AMD64_DRIVERS_SERIAL_H
#define _AMD64_DRIVERS_SERIAL_H

#include <types.h>

void serial_init();
uint8 serial_is_transmit_empty();
void serial_write_char(char c);
void serial_write(const char* s);
void serial_write_hex(uint64 n);

#endif