#include <io.h>
#include <system.h>
#include <string.h>
#include <keyboard.h>
#include "sha256.h"

int main(void) {
    if (kbd_init() < 0) return 1;

    while (1) {
        kbd_flush();
        
        char username[128];
        int pos = 0;
        puts("login: ");
        
        while (1) {
            char c = kbd_getchar();
            if (c == 0) continue;
            
            if (c == '\b') {
                if (pos > 0) {
                    pos--;
                    puts("\b \b");
                }
                continue;
            }
            
            putc(c);
            
            if (c == '\n') {
                username[pos] = '\0';
                break;
            } else {
                username[pos++] = c;
            }
        }
        
        char passwd[256];
        pos = 0;
        puts("password: ");
        while (1) {
            char c = kbd_getchar();
            if (c == 0) continue;
            
            if (c == '\b') {
                if (pos > 0) {
                    pos--;
                    puts("\b \b");
                }
                continue;
            }
            
            if (c == '\n') {
                passwd[pos] = '\0';
                break;
            } else {
                passwd[pos++] = c;
            }

            putc('*');
        }

        putc('\n');
        
        // TODO: actual passwd logic lol
        if (strcmp(username, passwd) == 0) {
            int pid = spawn("$files/system/binaries/shell", 0, NULL);
            wait(pid);
        }
    }
}