#include <system.h>
#include <io.h>
#include <string.h>
#include <keyboard.h>

#define HIST_MAX 16
#define LINE_MAX 128

static char history[HIST_MAX][LINE_MAX];
static int hist_count = 0;
static int hist_pos = -1;

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

// \033L / \033R move cursor left/right
static void cursor_left(void) {
    static const char seq[] = { 27, 'L' };
    handle_write(__stdout, seq, sizeof(seq));
}

static void cursor_right(void) {
    static const char seq[] = { 27, 'R' };
    handle_write(__stdout, seq, sizeof(seq));
}

static void redraw_buffer(const char *buf, int len) {
    for (int i = 0; i < len; i++) putc(buf[i]);
}

static void history_add(const char *line) {
    if (line[0] == '\0') return;
    if (hist_count > 0 && streq(history[hist_count - 1], line)) return;
    if (hist_count < HIST_MAX) {
        strncpy(history[hist_count], line, LINE_MAX - 1);
        history[hist_count][LINE_MAX - 1] = '\0';
        hist_count++;
    } else {
        //history full, shift oldest entries out
        for (int i = 0; i < HIST_MAX - 1; i++)
            memcpy(history[i], history[i + 1], LINE_MAX);
        strncpy(history[HIST_MAX - 1], line, LINE_MAX - 1);
        history[HIST_MAX - 1][LINE_MAX - 1] = '\0';
    }
    hist_pos = hist_count;
}

static const char *history_navigate(int direction) {
    if (hist_count == 0) return NULL;
    int new_pos = hist_pos + direction;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > hist_count) new_pos = hist_count;
    hist_pos = new_pos;
    if (hist_pos >= hist_count) return "";
    return history[hist_pos];
}

static void cmd_help(void) {
    puts("Commands: help, echo, cd, pwd, spawn, wm, dir, exit\n");
}

static void cmd_spawn(char *path) {
    if (!path) {
        puts("Usage: spawn <path>\n");
        return;
    }
    context_spawn_entry_t kctx = {
        .key          = "keyboard",
        .type         = CONTEXT_VALUE_OBJECT,
        .flags        = 0,
        .value_len    = 0,
        .value.handle = kbd_handle(),
    };
    int pid = spawn_ctx(path, 0, NULL, &kctx, 1);
    if (pid < 0) {
        printf("spawn: failed to start %s\n", path);
    } else {
        proc_set_console_foreground((uintptr)pid);
        printf("spawn: started %s (PID %d)\n", path, pid);
        kbd_close();  //release our slot; child owns the keyboard now
        int code = wait(pid);
        kbd_init();   //reclaim from namespace after child exits
        kbd_flush();
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
    if (chdir(path) < 0)
        printf("cd: failed to change directory to '%s'\n", path);
}

static void cmd_pwd(void) {
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) < 0)
        puts("pwd: failed to get current directory\n");
    else {
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
        char path[128];
        int cmd_len = strlen(cmd);
        if (cmd_len > 0 && cmd_len < 64) {
            snprintf(path, sizeof(path), "$files/system/binaries/%s", cmd);
            char *args_list[16];
            int argc = 1;
            args_list[0] = cmd;
            char *token;
            while ((token = strtok(NULL, " \t\n")) && argc < 15)
                args_list[argc++] = token;
            args_list[argc] = NULL;
            //pass keyboard to child via context then give up our slot
            context_spawn_entry_t kctx = {
                .key          = "keyboard",
                .type         = CONTEXT_VALUE_OBJECT,
                .flags        = 0,
                .value_len    = 0,
                .value.handle = kbd_handle(),
            };
            int pid = spawn_ctx(path, argc, args_list, &kctx, 1);
            if (pid < 0) {
                printf("Unknown command: %s\n", cmd);
            } else {
                proc_set_console_foreground((uintptr)pid);
                kbd_close();  //release keyboard; child owns it now
                int code = wait(pid);
                kbd_init();   //reclaim after child exits
                kbd_flush();
                proc_set_console_foreground(0);
                shell_reset_terminal();
                if (code == 141)
                    printf("Page fault; process killed.\n");
                else if (code != 0)
                    printf("Child died with error code %d\n", code);
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

    char buffer[LINE_MAX];
    int len = 0;
    int cur = 0;
    hist_pos = hist_count;

    shell_show_prompt();
    while (1) {
        kbd_event_t ev;
        if (kbd_read(&ev) < 0) continue;
        if (!ev.pressed) continue;

        //ctrl+c
        if ((ev.mods & KBD_MOD_CTRL) && (ev.codepoint == 'c' || ev.codepoint == 'C')) {
            while (cur < len) { cur++; cursor_right(); }
            for (int i = 0; i < len; i++) puts("\b \b");
            len = 0;
            cur = 0;
            putc('\n');
            hist_pos = hist_count;
            shell_show_prompt();
            continue;
        }

        if ((ev.codepoint & 0xE000) == 0xE000) {
            uint8 ext = ev.codepoint & 0xFF;

            //up arrow
            if (ext == 0x48) {
                const char *h = history_navigate(-1);
                if (h) {
                    int new_len = strlen(h);
                    if (new_len >= LINE_MAX) new_len = LINE_MAX - 1;
                    while (cur < len) { cur++; cursor_right(); }
                    for (int i = 0; i < len; i++) puts("\b \b");
                    len = new_len;
                    memcpy(buffer, h, (size)len);
                    buffer[len] = '\0';
                    cur = len;
                    redraw_buffer(buffer, len);
                }
                continue;
            }

            //down arrow
            if (ext == 0x50) {
                const char *h = history_navigate(1);
                if (h) {
                    int new_len = strlen(h);
                    if (new_len >= LINE_MAX) new_len = LINE_MAX - 1;
                    while (cur < len) { cur++; cursor_right(); }
                    for (int i = 0; i < len; i++) puts("\b \b");
                    len = new_len;
                    memcpy(buffer, h, (size)len);
                    buffer[len] = '\0';
                    cur = len;
                    redraw_buffer(buffer, len);
                }
                continue;
            }

            //left arrow
            if (ext == 0x4B) {
                if (cur > 0) {
                    cur--;
                    cursor_left();
                }
                continue;
            }

            //right arrow
            if (ext == 0x4D) {
                if (cur < len) {
                    cur++;
                    cursor_right();
                }
                continue;
            }

            //home
            if (ext == 0x47) {
                while (cur > 0) {
                    cur--;
                    cursor_left();
                }
                continue;
            }

            //end
            if (ext == 0x4F) {
                while (cur < len) {
                    cur++;
                    cursor_right();
                }
                continue;
            }

            //delete
            if (ext == 0x53) {
                if (cur < len) {
                    for (int i = cur; i < len; i++)
                        buffer[i] = buffer[i + 1];
                    len--;
                    redraw_buffer(buffer + cur, len - cur);
                    putc(' ');
                    int back = len - cur + 1;
                    while (back-- > 0) cursor_left();
                }
                continue;
            }
            continue;
        }

        //ctrl+p
        if ((ev.mods & KBD_MOD_CTRL) && ev.codepoint == 'p') {
            const char *h = history_navigate(-1);
            if (h) {
                int new_len = strlen(h);
                if (new_len >= LINE_MAX) new_len = LINE_MAX - 1;
                while (cur < len) { cur++; cursor_right(); }
                for (int i = 0; i < len; i++) puts("\b \b");
                len = new_len;
                memcpy(buffer, h, (size)len);
                buffer[len] = '\0';
                cur = len;
                redraw_buffer(buffer, len);
            }
            continue;
        }

        //ctrl+n
        if ((ev.mods & KBD_MOD_CTRL) && ev.codepoint == 'n') {
            const char *h = history_navigate(1);
            if (h) {
                int new_len = strlen(h);
                if (new_len >= LINE_MAX) new_len = LINE_MAX - 1;
                while (cur < len) { cur++; cursor_right(); }
                for (int i = 0; i < len; i++) puts("\b \b");
                len = new_len;
                memcpy(buffer, h, (size)len);
                buffer[len] = '\0';
                cur = len;
                redraw_buffer(buffer, len);
            }
            continue;
        }

        //ctrl+a
        if ((ev.mods & KBD_MOD_CTRL) && ev.codepoint == 'a') {
            while (cur > 0) {
                cur--;
                cursor_left();
            }
            continue;
        }

        //ctrl+e
        if ((ev.mods & KBD_MOD_CTRL) && ev.codepoint == 'e') {
            while (cur < len) {
                cur++;
                cursor_right();
            }
            continue;
        }

        //ctrl+u
        if ((ev.mods & KBD_MOD_CTRL) && ev.codepoint == 'u') {
            for (int i = 0; i < cur; i++) puts("\b \b");
            for (int i = 0; i < len - cur; i++)
                buffer[i] = buffer[cur + i];
            len -= cur;
            cur = 0;
            redraw_buffer(buffer, len);
            int back = len;
            while (back-- > 0) cursor_left();
            continue;
        }

        //ctrl+k
        if ((ev.mods & KBD_MOD_CTRL) && ev.codepoint == 'k') {
            int erased = len - cur;
            for (int i = 0; i < erased; i++) putc(' ');
            while (erased-- > 0) cursor_left();
            len = cur;
            buffer[len] = '\0';
            continue;
        }

        //ctrl+w
        if ((ev.mods & KBD_MOD_CTRL) && ev.codepoint == 'w') {
            if (cur == 0) continue;
            int word_start = cur - 1;
            while (word_start > 0 && buffer[word_start] == ' ') word_start--;
            while (word_start > 0 && buffer[word_start - 1] != ' ') word_start--;
            int n = cur - word_start;
            for (int i = word_start; i < len - n; i++)
                buffer[i] = buffer[i + n];
            len -= n;
            cur = word_start;
            redraw_buffer(buffer + cur, len - cur);
            for (int i = 0; i < n; i++) putc(' ');
            int back = len - cur + n;
            while (back-- > 0) cursor_left();
            continue;
        }

        //ctrl alt filter
        if (ev.mods & (KBD_MOD_CTRL | KBD_MOD_ALT))
            continue;

        char c = (char)ev.codepoint;
        if (c == 0) continue;

        //backspace
        if (c == '\b') {
            if (cur > 0) {
                cur--;
                for (int i = cur; i < len; i++)
                    buffer[i] = buffer[i + 1];
                len--;
                cursor_left();
                redraw_buffer(buffer + cur, len - cur);
                putc(' ');
                int back = len - cur + 1;
                while (back-- > 0) cursor_left();
            }
            continue;
        }

        if (c == '\n' || len >= LINE_MAX - 2) {
            buffer[len] = '\0';
            if (c == '\n') putc('\n');
            history_add(buffer);
            process_command(buffer);
            len = 0;
            cur = 0;
            hist_pos = hist_count;
            shell_show_prompt();
        } else {
            if (len < LINE_MAX - 1) {
                for (int i = len; i > cur; i--)
                    buffer[i] = buffer[i - 1];
                buffer[cur] = c;
                len++;
                cur++;
                putc(c);
                redraw_buffer(buffer + cur, len - cur);
                int back = len - cur;
                while (back-- > 0) cursor_left();
            }
        }
    }

    return 0;
}
