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
    
    puts("[shell] ready. Type 'help' for commands:\n");
    
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
        
        putc(c);
        buffer[l++] = c;
        
        if (c == '\n' || l >= 126) {
            buffer[l] = '\0';
            char *cmd = strtok(buffer, " \t\n");
            if (cmd) {
                if (streq(cmd, "help")) {
                    puts("Available commands: help, echo, exit\n");
                } else if (streq(cmd, "echo")) {
                    char *arg = strtok(0, "\n");
                    if (arg) puts(arg);
                    puts("\n");
                } else if (streq(cmd, "exit")) {
                    puts("Goodbye!\n");
                    exit(0);
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
