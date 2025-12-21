#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <arch/types.h>

void serial_init(void);
void serial_init_object(void);  //call after heap init
void serial_write_char(char c);
void serial_write(const char* s);
void serial_write_hex(uint64 n);

#endif
