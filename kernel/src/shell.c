#include <drivers/vt/vt.h>
#include <drivers/rtc.h>
#include <obj/handle.h>
#include <fs/fs.h>
#include <lib/io.h>
#include <string.h>

void shell(void) {
    vt_t *vt = vt_get_active();
    if (!vt) return;
    
    for (;;) {
        vt_print(vt, "[] ");
        vt_flush(vt);

        char buffer[128] = {0};
        uint8 i = 0;
        
        while (1) {
            vt_event_t event;
            vt_wait_event(vt, &event);
            
            //only handle key presses
            if (event.type != VT_EVENT_KEY || !event.pressed) continue;
            
            char c = (char)event.codepoint;
            if (!c) continue;  //non-printable
            
            if (c == '\b') {
                if (i > 0) { 
                    i--;
                    vt_putc(vt, '\b');
                    vt_flush(vt); 
                }
            } else {
                if (i >= sizeof(buffer) - 1) { buffer[i] = 0; break; }
                vt_putc(vt, c);
                vt_flush(vt);
                buffer[i++] = c;
                if (c == '\n') { buffer[i - 1] = '\0'; break; }
            }
        }

        if (strcmp(buffer, "help") == 0) {
            vt_print(vt, "Available commands: help, time, ls, cat, stat\n");
        } else if (strcmp(buffer, "time") == 0) {
            rtc_time_t time;
            rtc_get_time(&time);
            char buf[64];
            snprintf(buf, sizeof(buf), "Current time: %d:%d:%d %d/%d/%d\n", 
                time.hour, time.minute, time.second,
                time.day, time.month, time.year);
            vt_print(vt, buf);
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
                    for (int j = 0; j < count; j++) {
                        char line[64];
                        snprintf(line, sizeof(line), "  %s%s\n", entries[j].name, 
                               entries[j].type == FS_TYPE_DIR ? "/" : "");
                        vt_print(vt, line);
                    }
                } else {
                    vt_print(vt, "  (empty or not a directory)\n");
                }
                handle_close(h);
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "ls: cannot access '%s'\n", arg);
                vt_print(vt, msg);
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
                    vt_print(vt, data);
                }
                vt_print(vt, "\n");
                handle_close(h);
            } else {
                char msg[64];
            snprintf(msg, sizeof(msg), "cat: cannot open '%s'\n", arg);
                vt_print(vt, msg);
            }
        } else if (strncmp(buffer, "stat ", 5) == 0) {
            //show file stats
            const char *arg = buffer + 5;
            char path[128];
            snprintf(path, sizeof(path), "$files%s", arg);
            stat_t st;
            if (handle_stat(path, &st) == 0) {
                const char *type_str = st.type == FS_TYPE_FILE ? "file" : 
                                       st.type == FS_TYPE_DIR ? "dir" : "other";
                char msg[128];
                snprintf(msg, sizeof(msg), "%s: %s, %zu bytes\n", arg, type_str, st.size);
                vt_print(vt, msg);
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "stat: cannot stat '%s'\n", arg);
                vt_print(vt, msg);
            }
        } else if (strcmp(buffer, "r") == 0) {
            extern void rmain(void);
            rmain();
        } else if (buffer[0] != '\0') {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s: command not found\n", buffer);
            vt_print(vt, msg);
        }
        
        vt_flush(vt);
    }
}