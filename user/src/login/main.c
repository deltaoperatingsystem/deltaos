#include <io.h>
#include <system.h>
#include "user.h"
#include <keyboard.h>
#include <stdlib.h>

int main(void) {
    if (kbd_init() < 0) return 1;
    
    // default root password, can change lol
    struct passwd* root = get_user("root");
    if (root == NULL) {
        if (create_user("root", "toor") < 0) {
            puts("Failed to create root user\n");
        }
    } else {
        free(root);
    }
    
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
            
            if (c == '\n' || pos >= 128) {
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
            
            if (c == '\n' || pos >= 256) {
                passwd[pos] = '\0';
                break;
            } else {
                passwd[pos++] = c;
            }

            putc('*');
        }

        putc('\n');
        
        enum verif_stat vstat = verify_user(username, passwd);
        if (vstat == V_VALID) {
            int pid = spawn("$files/system/binaries/shell", 0, NULL);
            if (pid < 0) {
                puts("Failed to spawn shell!\n");
                continue;
            }
            wait(pid);
        } else {
            switch (vstat) {
                case V_EWPWD: puts("Wrong password\n"); continue;
                case V_ENUSR: puts("Unknown user\n"); continue;
                case V_EINTR: puts("Internal error in authentication\n"); continue;
                default: continue;
            }
        }
    }
}
