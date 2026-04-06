#include <curses.h>
#include <io.h>
#include <string.h>

#define HISTORY_LINES 16
#define INPUT_MAX 128

static char history[HISTORY_LINES][INPUT_MAX];
static uint32 history_len = 0;
static char input[INPUT_MAX];
static uint32 input_len = 0;

static void push_history(const char *line) {
    if (!line || !*line) return;

    if (history_len < HISTORY_LINES) {
        strncpy(history[history_len], line, INPUT_MAX - 1);
        history[history_len][INPUT_MAX - 1] = '\0';
        history_len++;
        return;
    }

    for (uint32 i = 1; i < HISTORY_LINES; i++) {
        strcpy(history[i - 1], history[i]);
    }
    strncpy(history[HISTORY_LINES - 1], line, INPUT_MAX - 1);
    history[HISTORY_LINES - 1][INPUT_MAX - 1] = '\0';
}

static void redraw(void) {
    curses_reset_style();
    curses_set_bg(CURSES_RGB(0, 0, 0));
    curses_set_fg(CURSES_RGB(235, 235, 235));
    curses_clear();

    uint32 rows = curses_get_rows();
    uint32 cols = curses_get_cols();
    if (rows == 0 || cols == 0) return;

    uint32 shell_h = rows >= 16 ? rows - 6 : rows;
    uint32 log_x = 2;
    uint32 log_y = 3;
    uint32 log_w = 0;
    uint32 side_x = 0;
    uint32 side_w = 0;
    uint32 log_h = shell_h > log_y + 4 ? shell_h - log_y : 6;
    uint32 input_y = rows > 5 ? rows - 4 : rows - 2;

    if (cols > 28) {
        log_w = cols - 24;
        side_x = log_x + log_w + 1;
        side_w = cols > side_x + 2 ? cols - side_x - 2 : 0;
    } else if (cols > 6) {
        log_w = cols - 4;
    }

    curses_set_fg(CURSES_RGB(120, 220, 255));
    curses_center_text(0, "DeltaOS TUI demo");

    curses_set_fg(CURSES_RGB(180, 180, 180));
    curses_center_text(1, "Type text, press Enter to log it, or type 'quit' to exit.");

    if (log_w >= 12 && log_h >= 4) {
        curses_set_fg(CURSES_RGB(255, 200, 120));
        curses_draw_box(log_x, log_y, log_w, log_h);
        curses_write_at(log_x + 2, log_y, " log ", log_w > 4 ? log_w - 4 : 0);

        uint32 log_inner_x = log_x + 2;
        uint32 log_inner_y = log_y + 1;
        uint32 log_inner_w = log_w > 4 ? log_w - 4 : 0;
        uint32 log_inner_h = log_h > 2 ? log_h - 2 : 0;
        uint32 visible = history_len < log_inner_h ? history_len : log_inner_h;
        uint32 first = history_len > visible ? history_len - visible : 0;

        curses_set_fg(CURSES_RGB(220, 220, 220));
        for (uint32 i = 0; i < visible; i++) {
            curses_write_at(log_inner_x, log_inner_y + i, history[first + i], log_inner_w);
        }
    }

    if (side_w >= 14 && log_h >= 10) {
        curses_set_fg(CURSES_RGB(160, 220, 255));
        curses_draw_box(side_x, log_y, side_w, 7);
        curses_write_at(side_x + 2, log_y, " controls ", side_w > 4 ? side_w - 4 : 0);

        curses_set_fg(CURSES_RGB(210, 210, 210));
        curses_write_at(side_x + 2, log_y + 2, "enter  submit line", side_w > 4 ? side_w - 4 : 0);
        curses_write_at(side_x + 2, log_y + 3, "backsp erase char", side_w > 4 ? side_w - 4 : 0);
        curses_write_at(side_x + 2, log_y + 4, "quit   leave demo", side_w > 4 ? side_w - 4 : 0);

        curses_set_fg(CURSES_RGB(180, 255, 180));
        curses_draw_box(side_x, log_y + 8, side_w, 6);
        curses_write_at(side_x + 2, log_y + 8, " status ", side_w > 4 ? side_w - 4 : 0);

        char status_line[64];
        snprintf(status_line, sizeof(status_line), "entries: %lu", (unsigned long)history_len);
        curses_set_fg(CURSES_RGB(230, 230, 230));
        curses_write_at(side_x + 2, log_y + 10, status_line, side_w > 4 ? side_w - 4 : 0);
        snprintf(status_line, sizeof(status_line), "input: %lu chars", (unsigned long)input_len);
        curses_write_at(side_x + 2, log_y + 11, status_line, side_w > 4 ? side_w - 4 : 0);
    }

    if (rows >= 5 && cols >= 12) {
        curses_set_fg(CURSES_RGB(160, 255, 160));
        curses_draw_box(2, input_y, cols - 4, 3);
        curses_write_at(4, input_y, " command ", cols > 8 ? cols - 8 : 0);

        char prompt_line[INPUT_MAX + 3];
        snprintf(prompt_line, sizeof(prompt_line), "> %s", input);
        curses_set_fg(CURSES_RGB(255, 255, 255));
        curses_write_at(4, input_y + 1, prompt_line, cols > 8 ? cols - 8 : 0);
    }

    curses_flush();
}

int main(void) {
    if (curses_init() < 0) {
        puts("tui: failed to initialize curses\n");
        return 1;
    }

    redraw();

    while (1) {
        kbd_event_t ev;
        if (curses_read(&ev) < 0) break;

        if (curses_event_is_submit(&ev)) {
            input[input_len] = '\0';
            if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
                break;
            }
            push_history(input);
            input_len = 0;
            input[0] = '\0';
            redraw();
            continue;
        }

        if (curses_event_is_backspace(&ev)) {
            if (input_len > 0) {
                input_len--;
                input[input_len] = '\0';
                redraw();
            }
            continue;
        }

        if (curses_event_is_printable(&ev)) {
            if (input_len + 1 < INPUT_MAX) {
                input[input_len++] = curses_event_char(&ev);
                input[input_len] = '\0';
                redraw();
            }
        }
    }

    curses_shutdown();
    return 0;
}
