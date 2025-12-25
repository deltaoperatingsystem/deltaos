#include <lib/io.h>
#include <drivers/rtc.h>
#include <drivers/keyboard.h>
#include <drivers/console.h>

void shell(void) {
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
        } else if (strcmp(buffer, "r") == 0) {
            extern void rmain(void);
            rmain();
        }else if (buffer[0] != '\0') {
            printf("%s: command not found\n", buffer);
        }
    }
}