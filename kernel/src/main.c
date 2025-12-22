#include <arch/types.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <drivers/fb.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/rtc.h>
#include <drivers/serial.h>
#include <lib/string.h>
#include <lib/io.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kheap.h>
#include <obj/handle.h>
#include <obj/namespace.h>
#include <fs/tmpfs.h>
#include <fs/fs.h>
#include <proc/sched.h>

void kernel_main(void) {
    set_outmode(SERIAL);
    puts("kernel_main started\n");
    
    fb_init();
    fb_init_backbuffer();
    serial_init_object();
    keyboard_init();
    rtc_init();
    tmpfs_init();
    sched_init();
    
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
        
        //test object system
        handle_t h = handle_open("$devices/console", 0);
        if (h != INVALID_HANDLE) {
            handle_write(h, "Object system: working!\n", 24);
            handle_close(h);
        }

    }
    
    for (;;) {
        set_outmode(CONSOLE);
        puts("[] ");
        con_flush();  //flush prompt to screen

        char buffer[128] = {0};
        uint8 i = 0;
        while (1) {
            keyboard_wait();
            char c;
            if (!get_key(&c)) continue;
            if (c == '\b') {
                if (i > 0) { 
                    i--;
                    putc('\b');
                    con_flush(); 
                }
            } else {
                if (i >= sizeof(buffer) - 1) { buffer[i] = 0; break; }
                putc(c);
                con_flush();  //show typed char immediately
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
