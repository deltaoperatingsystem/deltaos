#include <arch/types.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <drivers/fb.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/rtc.h>
#include <kernel/device.h>
#include <lib/string.h>
#include <lib/io.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kheap.h>

void kernel_main(void) {
    set_outmode(SERIAL);
    puts("kernel_main started\n");
    
    fb_init();
    keyboard_init();
    rtc_init();
    
    if (fb_available()) {
        con_init();
        con_clear();
        set_outmode(CONSOLE);
        
        con_set_fg(FB_RGB(0, 255, 128));
        puts("DeltaOS Kernel\n");
        puts("==============\n\n");
        
        con_set_fg(FB_WHITE);
        puts("Console: initialized\n");
        printf("Timer: running @ %dHz\n", arch_timer_getfreq());
        
        struct device *con = device_find("console");
        if (con && con->ops->write) {
            con->ops->write(con, "Device manager: working!\n", 25);
        }
    }
    
    for (;;) {
        set_outmode(CONSOLE);
        puts("[] ");

        char buffer[128] = {0};
        uint8 i = 0;
        while (1) {
            keyboard_wait();
            char c;
            if (!get_key(&c)) continue;
            if (c == '\b') {
                if (i > 0) { i--; putc('\b'); }
            } else {
                if (i >= sizeof(buffer) - 1) { buffer[i] = 0; break; }
                putc(c);
                buffer[i++] = c;
                if (c == '\n') { buffer[i - 1] = '\0'; break; }
            }
        }

        if (strcmp(buffer, "help") == 0) {
            puts("Available commands: help, time\n");
        } else if (strcmp(buffer, "time") == 0) {
            rtc_time_t time;
            rtc_get_time(&time);
            printf("Current time: %d:%d:%d %d/%d/%d\n", 
                time.hour, time.minute, time.second,
                time.day, time.month, time.year);
        } else if (buffer[0] != '\0') {
            printf("%s: command not found\n", buffer);
        }
    }
}
