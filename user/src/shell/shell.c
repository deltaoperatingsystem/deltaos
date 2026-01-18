#include <system.h>
#include <io.h>
#include <string.h>
#include <keyboard.h>

static void cmd_help(void) {
    puts("Commands: help, echo, cd, pwd, spawn, wm, ls, exit\n");
}

static void cmd_echo(char *args) {
    if (args) puts(args);
    puts("\n");
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
        printf("spawn: started %s (PID %d)\n", path, child);
        wait(child);
    }
}

static void cmd_wm(void) {
    int child = spawn("$files/system/binaries/wm", 0, NULL);
    if (child < 0) {
        printf("wm: failed to start (error %d)\n", child);
    } else {
        printf("wm: started (PID %d)\n", child);
        wait(child);
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
    } else if (streq(cmd, "echo")) {
        cmd_echo(strtok(NULL, "\n"));
    } else if (streq(cmd, "cd")) {
        cmd_cd(strtok(NULL, " \t\n"));
    } else if (streq(cmd, "pwd")) {
        cmd_pwd();
    } else if (streq(cmd, "spawn")) {
        cmd_spawn(strtok(NULL, " \t\n"));
    } else if (streq(cmd, "wm")) {
        cmd_wm();
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
                wait(child);
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
    puts("[shell] ready. Type 'help' for commands.\n");
    
    char buffer[128];
    int pos = 0;
    
    //show initial prompt with CWD
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) >= 0) {
        printf("%s> ", cwd);
    } else {
        puts("> ");
    }
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
        
        if (c == '\n' || pos >= 126) {
            buffer[pos] = '\0';
            process_command(buffer);
            pos = 0;
            
            //show prompt with CWD
            char cwd[256];
            if (getcwd(cwd, sizeof(cwd)) >= 0) {
                printf("%s> ", cwd);
            } else {
                puts("> ");
            }
        } else {
            buffer[pos++] = c;
        }
    }
    
    return 0;
}
