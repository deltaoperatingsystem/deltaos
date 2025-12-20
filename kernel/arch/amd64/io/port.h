#ifndef IO_PORT_H
#define IO_PORT_H

#include <types.h>

uint8 inb(uint16 port);
void outb(uint16 port, uint8 value);

#endif