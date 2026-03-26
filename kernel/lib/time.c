#include <arch/cpu.h>
#include <arch/timer.h>
#include <arch/percpu.h>
#include <proc/sched.h>

static void wait_ticks(uint64 ticks_needed) {
    if (ticks_needed == 0) return;

    uint64 start = arch_timer_get_ticks();
    while ((arch_timer_get_ticks() - start) < ticks_needed) {
        percpu_t *cpu = percpu_get();
        if (cpu && cpu->sched_running) {
            sched_yield();
        } else {
            arch_halt();
        }
    }
}

void usleep(uint32 microseconds) {
    uint32 freq = arch_timer_getfreq(); // Hz
    uint64 ticks_needed = ((uint64)freq * microseconds) / 1000000;
    if (microseconds && ticks_needed == 0) ticks_needed = 1;

    wait_ticks(ticks_needed);
}

void sleep(uint32 milliseconds) {
    uint32 freq = arch_timer_getfreq(); // Hz
    uint64 ticks_needed = ((uint64)freq * milliseconds) / 1000;
    if (milliseconds && ticks_needed == 0) ticks_needed = 1;

    wait_ticks(ticks_needed);
}
