#include <lib/io.h>
#include <drivers/rtc.h>
#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <obj/handle.h>
#include <fs/vfs.h>

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
            puts("Available commands: help, time, ls, cat\n");
        } else if (strcmp(buffer, "time") == 0) {
            rtc_time_t time;
            rtc_get_time(&time);
            printf("Current time: %d:%d:%d %d/%d/%d\n", 
                time.hour, time.minute, time.second,
                time.day, time.month, time.year);
        } else if (strncmp(buffer, "ls ", 3) == 0 || strcmp(buffer, "ls") == 0) {
            //list directory contents
            const char *arg = buffer[2] ? buffer + 3 : "/initrd";
            char path[128];
            snprintf(path, sizeof(path), "$files%s", arg);
            handle_t h = handle_open(path, 0);
            if (h != INVALID_HANDLE) {
                dirent_t entries[16];
                int count = handle_readdir(h, entries, 16);
                if (count > 0) {
                    for (int i = 0; i < count; i++) {
                        printf("  %s%s\n", entries[i].name, 
                               entries[i].type == FS_TYPE_DIR ? "/" : "");
                    }
                } else {
                    puts("  (empty or not a directory)\n");
                }
                handle_close(h);
            } else {
                printf("ls: cannot access '%s'\n", arg);
            }
        } else if (strncmp(buffer, "cat ", 4) == 0) {
            //read file contents
            const char *arg = buffer + 4;
            char path[128];
            snprintf(path, sizeof(path), "$files%s", arg);
            handle_t h = handle_open(path, 0);
            if (h != INVALID_HANDLE) {
                char data[256];
                ssize n;
                while ((n = handle_read(h, data, sizeof(data) - 1)) > 0) {
                    data[n] = '\0';
                    puts(data);
                }
                puts("\n");
                handle_close(h);
            } else {
                printf("cat: cannot open '%s'\n", arg);
            }
        } else if (strcmp(buffer, "r") == 0) {
            extern void rmain(void);
            rmain();
        } else if (buffer[0] != '\0') {
            printf("%s: command not found\n", buffer);
        }
    }
}