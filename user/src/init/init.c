#include <system.h>
#include <io.h>
#include <string.h>

//keyboard event structure (matches kernel kbd_event_t)
typedef struct {
    uint8  keycode;
    uint8  mods;
    uint8  pressed;
    uint8  _pad;
    uint32 codepoint;
} kbd_event_t;

static int32 kbd_channel = INVALID_HANDLE;

void shell(void) {
    //open keyboard channel
    kbd_channel = get_obj(INVALID_HANDLE, "$devices/keyboard/channel", RIGHT_READ | RIGHT_WRITE);
    if (kbd_channel == INVALID_HANDLE) {
        puts("[shell] failed to get keyboard channel\n");
        return;
    }
    
    //flush any stale keyboard events from boot
    kbd_event_t flush_event;
    while (channel_try_recv(kbd_channel, &flush_event, sizeof(flush_event)) > 0) {
        //discard
    }
    
    puts("[shell] ready. Type something:\n");
    
    char buffer[128];
    int l = 0;

    puts("> ");
    while (true) {
        //blocking recv - waits until key is pressed
        kbd_event_t event;
        int len = channel_recv(kbd_channel, &event, sizeof(event));
        
        if (len <= 0) {
            yield();  //fallback if recv fails
            continue;
        }
        
        char c = (char)event.codepoint;
        if (c == 0) continue;  //non-printable
        
        if (c == '\b') {
            if (l > 0) {
                l--;
                puts("\b \b");
            }
            continue;
        }

        putc(c);
        buffer[l++] = c;
        
        if (c == '\n' || l >= 126) {
            buffer[l - 1] = '\0';
            char *cmd = strtok(buffer, " \t\n");
            if (cmd) {
                if (streq(cmd, "help")) {
                    puts("Available commands: help, echo, spawn, wm, exit\n");
                } else if (streq(cmd, "echo")) {
                    char *arg = strtok(0, "\n");
                    if (arg) puts(arg);
                    puts("\n");
                } else if (streq(cmd, "exit")) {
                    puts("Goodbye!\n");
                    exit(0);
                } else if (streq(cmd, "spawn")) {
                    char *path = strtok(NULL, " \t\n");
                    if (path) {
                        int child = spawn(path, 0, NULL);
                        if (child < 0) {
                            printf("spawn: failed to start %s (error %d)\n", path, child);
                        } else {
                            printf("spawn: started %s (PID %d)\n", path, child);
                            wait(child);
                        }
                    } else {
                        puts("Usage: spawn <path>\n");
                    }
                } else if (streq(cmd, "wm")) {
                    int child = spawn("$files/initrd/wm", 0, NULL);
                    if (child < 0) {
                        printf("wm: failed to start (error %d)\n", child);
                    } else {
                        printf("wm: started (PID %d)\n", child);
                        wait(child);
                    }
                } else {
                    puts("Unknown command: ");
                    puts(cmd);
                    puts("\n");
                }
            }
            l = 0;
            puts("> ");
        }
    }
}

int main(int argc, char *argv[]) {
    puts("[user] hello from userspace!\n");

    printf("[user] argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("[user] argv[%d] = %s\n", i, argv[i]);
    }

    int pid = (int)getpid();
    printf("[user] getpid() = %d\n", pid);

    shell();
    
    return 0;
}