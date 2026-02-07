#include <arch/amd64/io.h>

uint8 inb(uint16 port) {
    uint8 result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void outb(uint16 port, uint8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint16 inw(uint16 port) {
    uint16 result;
    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void outw(uint16 port, uint16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

uint32 inl(uint16 port) {
    uint32 result;
    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void outl(uint16 port, uint32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}