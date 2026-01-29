#include <arch/amd64/types.h>
#include <arch/amd64/io.h>
#include <arch/amd64/interrupts.h>
#include <lib/io.h>
#include <arch/amd64/int/apic.h>

#define PIT_CMD   0x43
#define PIT_CH0   0x40
#define PIT_BASE  1193182U

static volatile uint64 timer_ticks = 0;
static volatile uint32 timer_freq = 0;

void arch_timer_tick(void) {
    timer_ticks++;
}

uint64 arch_timer_get_ticks(void) {
    return timer_ticks;
}

void arch_timer_setfreq(uint32 hz) {
    if (hz == 0) return;
    timer_freq = hz;
    uint16 div = (uint16)(PIT_BASE / hz);
    outb(PIT_CMD, 0b00110110);
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
}

uint32 arch_timer_getfreq(void) {
    return timer_freq;
}

void arch_timer_init(uint32 hz) {
    if (apic_is_enabled()) {
        timer_freq = hz;
        apic_timer_init(hz);
    } else {
        printf("[pit] initializing @ %u Hz...\n", hz);
        arch_timer_setfreq(hz);
        interrupt_unmask(0);
    }
}