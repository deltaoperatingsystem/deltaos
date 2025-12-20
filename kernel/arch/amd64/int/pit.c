#include <types.h>
#include <io/port.h>
#include <int/pic.h>

#define PIT_CMD   0x43
#define PIT_CH0   0x40
#define PIT_BASE  1193182U

void pit_setfreq(uint32 hz) {
    if (hz == 0) return;
    uint16 div = (uint16)(PIT_BASE / hz);
    outb(PIT_CMD, 0b00110110);
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
}

void pit_init(uint32 hz) {
    pit_setfreq(hz);
    pic_clear_mask(0);
}