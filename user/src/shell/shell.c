#include <system.h>
#include <io.h>
#include <string.h>
#include <keyboard.h>

static void shell_reset_terminal(void) {
    if (__stdout == INVALID_HANDLE) return;

    static const char reset_seq[] = {
        27, 'f', 'F', 'F', 'F', 'F', 'F', 'F',
        27, 'b', '0', '0', '0', '0', '0', '0',
        27, 'v', '1'
    };

    handle_write(__stdout, reset_seq, sizeof(reset_seq));
}

static void shell_show_prompt(void) {
    proc_set_console_foreground(0);
    shell_reset_terminal();

    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) >= 0) {
        printf("%s> ", cwd);
    } else {
        puts("> ");
    }
}

static void cmd_help(void) {
    puts("Commands: help, echo, cd, pwd, spawn, wm, dir, exit\n");
}


static void cmd_spawn(char *path) {
    if (!path) {
        puts("Usage: spawn <path>\n");
        return;
    }
    
    int child = spawn(path, 0, NULL);
    if (child < 0) {
        printf("spawn: failed to start %s (error %d)\n", path, child);
    } else {
        proc_set_console_foreground((uintptr)child);
        printf("spawn: started %s (PID %d)\n", path, child);
        int code = wait(child);
        proc_set_console_foreground(0);
        shell_reset_terminal();
        printf("spawn: child died with code %d\n", code);
    }
}

static void cmd_cd(char *path) {
    if (!path) {
        puts("Usage: cd <path>\n");
        return;
    }
    
    if (chdir(path) < 0) {
        printf("cd: failed to change directory to '%s'\n", path);
    }
}

static void cmd_pwd(void) {
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) < 0) {
        puts("pwd: failed to get current directory\n");
    } else {
        puts(cwd);
        puts("\n");
    }
}

static void process_command(char *line) {
    char *cmd = strtok(line, " \t\n");
    if (!cmd) return;
    
    if (streq(cmd, "help")) {
        cmd_help();
    } else if (streq(cmd, "cd")) {
        cmd_cd(strtok(NULL, " \t\n"));
    } else if (streq(cmd, "pwd")) {
        cmd_pwd();
    } else if (streq(cmd, "spawn")) {
        cmd_spawn(strtok(NULL, " \t\n"));
    } else if (streq(cmd, "exit")) {
        puts("Goodbye!\n");
        exit(0);
    } else {
        //try to spawn from /system/binaries
        char path[128];
        int len = strlen(cmd);
        
        if (len > 0 && len < 64) {
            snprintf(path, sizeof(path), "$files/system/binaries/%s", cmd);
            
            //collect arguments
            char *args_list[16];
            int argc = 1;
            args_list[0] = cmd;
            
            char *token;
            while ((token = strtok(NULL, " \t\n")) && argc < 15) {
                args_list[argc++] = token;
            }
            args_list[argc] = NULL;
            
            int child = spawn(path, argc, args_list);
            if (child < 0) {
                printf("Unknown command: %s\n", cmd);
            } else {
                proc_set_console_foreground((uintptr)child);
                int code = wait(child);
                proc_set_console_foreground(0);
                shell_reset_terminal();
                if (code == 141) {
                    printf("Page fault; process killed.\n");
                }
                else if (code != 0) {
                    printf("Child died with error code %d\n", code);
                }
            }
        } else {
            printf("Unknown command: %s\n", cmd);
        }
    }
}

int main(int argc, char *argv[]) {
    if (kbd_init() < 0) {
        puts("[shell] failed to initialize keyboard\n");
        return 1;
    }
    
    kbd_flush();
    proc_set_console_foreground(0);
    puts("[shell] ready. Type 'help' for commands.\n");
    
    char buffer[128];
    int pos = 0;
    
    //show initial prompt with CWD
    shell_show_prompt();
    while (1) {
        kbd_event_t ev;
        if (kbd_read(&ev) < 0) continue;
        if (!ev.pressed) continue;

        //for ctrl+C in the shell f the line is empty do nothing otherwise erase the current input
        if ((ev.mods & KBD_MOD_CTRL) && (ev.codepoint == 'c' || ev.codepoint == 'C')) {
            while (pos > 0) {
                pos--;
                puts("\b \b");
            }
            continue;
        }

        if (ev.mods & (KBD_MOD_CTRL | KBD_MOD_ALT)) {
            continue;
        }

        char c = (char)ev.codepoint;
        if (c == 0) continue;

        if (c == '\b') {
            if (pos > 0) {
                pos--;
                puts("\b \b");
            }
            continue;
        }

        putc(c);

        if (c == '\n' || pos >= 126) {
            buffer[pos] = '\0';
            process_command(buffer);
            pos = 0;
            
            //show prompt with CWD
            shell_show_prompt();
        } else {
            buffer[pos++] = c;
        }
    }
    
    return 0;
}
