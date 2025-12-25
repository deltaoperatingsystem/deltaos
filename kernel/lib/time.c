#include <arch/timer.h>

void usleep(uint32 microseconds) {
    uint32 freq = arch_timer_getfreq(); // Hz
    uint64 ticks_needed = ((uint64)freq * microseconds) / 1000000;

    uint64 start = arch_timer_get_ticks();
    while ((arch_timer_get_ticks() - start) < ticks_needed);
}

void sleep(uint32 milliseconds) {
    uint32 freq = arch_timer_getfreq(); // Hz
    uint64 ticks_needed = ((uint64)freq * milliseconds) / 1000;

    uint64 start = arch_timer_get_ticks();
    while ((arch_timer_get_ticks() - start) < ticks_needed);
}
