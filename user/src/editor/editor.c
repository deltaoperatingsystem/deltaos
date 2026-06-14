/*
 *kybindings:
 * ctrl+s - save
 * ctrl+x - exit (prompts if unsaved)
 * ctrl+o - open file (enter filename at prompt)
 * ctrl+k - cut current line into clipboard
 * ctrl+u - paste clipboard
 * ctrl+g - go to line (enter number at prompt)
 * arrows - move cursor
 * home / end - start / end of line
 * pgup / pgdn - scroll one screen
 * backspace / del - delete character
 * enter - insert newline
 */

#include <curses.h>
#include <fs.h>
#include <io.h>
#include <mem.h>
#include <string.h>
#include <system.h>

//tunables
#define LINE_CAP_INIT   128
#define FILENAME_MAX    256
#define CLIP_MAX        4096
#define STATUS_TIMEOUT  120    //redraws before status msg clears

//extended key codepoints
#define KEY_EXT(x)   (0xE000u | (unsigned)(x))
#define KEY_UP       KEY_EXT(0x48)
#define KEY_DOWN     KEY_EXT(0x50)
#define KEY_LEFT     KEY_EXT(0x4B)
#define KEY_RIGHT    KEY_EXT(0x4D)
#define KEY_HOME     KEY_EXT(0x47)
#define KEY_END      KEY_EXT(0x4F)
#define KEY_PGUP     KEY_EXT(0x49)
#define KEY_PGDN     KEY_EXT(0x51)
#define KEY_DEL      KEY_EXT(0x53)
#define KEY_INS      KEY_EXT(0x52)

//colors
#define C_BG CURSES_RGB(15,  15,  20 )
#define C_TEXT CURSES_RGB(220, 220, 220)
#define C_LINENUM CURSES_RGB(90,  90,  120)
#define C_TITLEBAR_BG CURSES_RGB(30,  100, 180)
#define C_TITLEBAR_FG CURSES_RGB(255, 255, 255)
#define C_STATUSBAR_BG CURSES_RGB(30, 100, 180)
#define C_STATUSBAR_FG CURSES_RGB(255, 255, 255)
#define C_PROMPT_BG CURSES_RGB(25,  25,  60 )
#define C_PROMPT_FG CURSES_RGB(255, 255, 160)
#define C_HINT_KEY CURSES_RGB(0,   180, 255)
#define C_HINT_DESC CURSES_RGB(200, 200, 200)
#define C_MODIFIED CURSES_RGB(255, 160, 50 )
#define C_CURSOR_LINE CURSES_RGB(25,  25,  40 )

typedef struct {
    char *buf; //heap-allocated text, NUL-terminated
    uint32 len; //length without NUL
    uint32 cap; //allocated capacity including NUL
} line_t;

//editor state
static line_t *lines = NULL;
static uint32 line_cap = 0;
static uint32 line_count = 1; //always >= 1
static uint32 cur_row = 0; //cursor logical row
static uint32 cur_col = 0; //cursor logical column (byte offset)
static uint32 scroll_row = 0; //top visible row
static uint32 scroll_col = 0; //left visible column
static uint32 preferred_col = 0; //column to restore after vertical move
static bool modified = false;
static char filename[FILENAME_MAX] = "";

static char clipboard[CLIP_MAX]    = "";
static uint32 clip_len               = 0;

static char status_msg[256]        = "";
static int status_ttl             = 0;

//forward decls
static void editor_redraw(void);
static int editor_prompt(const char *prompt, char *out, uint32 bufsz);
static void editor_save(void);
static void editor_load(const char *path);
static void editor_status(const char *msg);

//ine management
static void line_ensure(line_t *l, uint32 needed) {
    if (needed < l->cap) return;
    uint32 new_cap = l->cap ? l->cap * 2 : LINE_CAP_INIT;
    while (new_cap <= needed) new_cap *= 2;
    char *nb = realloc(l->buf, new_cap);
    if (!nb) return;
    l->buf = nb;
    l->cap = new_cap;
}

static void line_init(line_t *l, const char *s, uint32 slen) {
    l->cap = 0;
    l->buf = NULL;
    l->len = 0;
    line_ensure(l, slen + 1);
    if (l->buf) {
        memcpy(l->buf, s, slen);
        l->buf[slen] = '\0';
        l->len = slen;
    }
}

static void line_free(line_t *l) {
    if (l->buf) { 
        free(l->buf); 
        l->buf = NULL; 
    }
    l->len = l->cap = 0;
}

static void line_insert(line_t *l, uint32 pos, char c) {
    line_ensure(l, l->len + 2);
    if (!l->buf) return;
    memmove(l->buf + pos + 1, l->buf + pos, l->len - pos + 1);
    l->buf[pos] = c;
    l->len++;
}

static void line_delete(line_t *l, uint32 pos) {
    if (pos >= l->len) return;
    memmove(l->buf + pos, l->buf + pos + 1, l->len - pos);
    l->len--;
}

//append s[0..slen) to line
static void line_append(line_t *l, const char *s, uint32 slen) {
    line_ensure(l, l->len + slen + 1);
    if (!l->buf) return;
    memcpy(l->buf + l->len, s, slen);
    l->len += slen;
    l->buf[l->len] = '\0';
}

//ensure lines array has room for at least `needed` entries
static void doc_ensure_cap(uint32 needed) {
    if (needed < line_cap) return;
    uint32 new_cap = line_cap ? line_cap * 2 : 64;
    while (new_cap <= needed) new_cap *= 2;
    line_t *nl = realloc(lines, new_cap * sizeof(line_t));
    if (!nl) return;
    memset(nl + line_cap, 0, (new_cap - line_cap) * sizeof(line_t));
    lines = nl;
    line_cap = new_cap;
}

//insert a blank line after index `after`
static void doc_insert_line_after(uint32 after) {
    doc_ensure_cap(line_count + 1);
    for (uint32 i = line_count; i > after + 1; i--) {
        lines[i] = lines[i - 1];
    }
    line_count++;
    line_t *nl = &lines[after + 1];
    nl->buf = NULL; nl->len = nl->cap = 0;
    line_init(nl, "", 0);
}

//remove line at index `idx`
static void doc_remove_line(uint32 idx) {
    if (line_count <= 1) return;
    line_free(&lines[idx]);
    for (uint32 i = idx; i < line_count - 1; i++) {
        lines[i] = lines[i + 1];
    }
    line_count--;
    lines[line_count].buf = NULL;
    lines[line_count].len = lines[line_count].cap = 0;
}

//cursor helpers
static void clamp_cursor(void) {
    if (cur_row >= line_count) cur_row = line_count - 1;
    if (cur_col > lines[cur_row].len) cur_col = lines[cur_row].len;
}

static void ensure_scroll(void) {
    uint32 rows = curses_get_rows();
    uint32 cols = curses_get_cols();
    uint32 edit_rows = (rows >= 4) ? rows - 3 : 1; //title + 1 status + 1 hint
    uint32 linenum_w = 5;
    uint32 edit_cols = (cols > linenum_w + 1) ? cols - linenum_w - 1 : 1;

    if (cur_row < scroll_row) {
        scroll_row = cur_row;
    }
    if (cur_row >= scroll_row + edit_rows) {
        scroll_row = cur_row - edit_rows + 1;
    }
    if (cur_col < scroll_col) {
        scroll_col = cur_col;
    }
    if (cur_col >= scroll_col + edit_cols) {
        scroll_col = cur_col - edit_cols + 1;
    }
}

//status message
static void editor_status(const char *msg) {
    strncpy(status_msg, msg, sizeof(status_msg) - 1);
    status_msg[sizeof(status_msg) - 1] = '\0';
    status_ttl = STATUS_TIMEOUT;
}

//endering
static void draw_titlebar(void) {
    uint32 cols = curses_get_cols();
    curses_set_bg(C_TITLEBAR_BG);
    curses_set_fg(C_TITLEBAR_FG);
    curses_fill_rect(0, 0, cols, 1, ' ');

    char title[FILENAME_MAX + 32];
    const char *name = filename[0] ? filename : "[New File]";
    snprintf(title, sizeof(title), " editor  \xe2\x80\xa2  %s%s",
             name, modified ? " [modified]" : "");

    curses_write_at(0, 0, title, cols);
}

static void draw_statusbar(void) {
    uint32 cols = curses_get_cols();
    uint32 rows = curses_get_rows();
    if (rows < 2) return;

    uint32 bar_row = rows - 2;
    curses_set_bg(C_STATUSBAR_BG);
    curses_set_fg(C_STATUSBAR_FG);
    curses_fill_rect(0, bar_row, cols, 1, ' ');

    char info[128];
    snprintf(info, sizeof(info), " Ln %lu/%lu  Col %lu",
             (unsigned long)(cur_row + 1),
             (unsigned long)line_count,
             (unsigned long)(cur_col + 1));
    curses_write_at(0, bar_row, info, cols / 2);

    if (status_ttl > 0) {
        status_ttl--;
        curses_set_fg(C_MODIFIED);
        uint32 msg_col = cols > 40 ? cols / 2 : 0;
        curses_write_at(msg_col, bar_row, status_msg, cols / 2);
    }
}

static void draw_hintbar(void) {
    uint32 cols = curses_get_cols();
    uint32 rows = curses_get_rows();
    if (rows < 1) return;

    uint32 hint_row = rows - 1;
    curses_set_bg(C_BG);
    curses_fill_rect(0, hint_row, cols, 1, ' ');

    typedef struct { const char *key; const char *desc; } hint_t;
    static const hint_t hints[] = {
        { "^X", "Exit"  },
        { "^S", "Save"  },
        { "^O", "Open"  },
        { "^K", "Cut"   },
        { "^U", "Paste" },
        { "^G", "GoLine"},
    };
    static const uint32 nhints = 6;

    uint32 x = 0;
    for (uint32 i = 0; i < nhints && x + 14 <= cols; i++) {
        curses_set_bg(C_BG);
        curses_set_fg(C_HINT_KEY);
        curses_write_at(x, hint_row, hints[i].key, 2);
        x += 2;
        curses_set_fg(C_HINT_DESC);
        char tmp[12];
        snprintf(tmp, sizeof(tmp), " %-5s  ", hints[i].desc);
        curses_write_at(x, hint_row, tmp, 8);
        x += 8;
    }
}

static void draw_text_area(void) {
    uint32 cols = curses_get_rows() > 0 ? curses_get_cols() : 80;
    uint32 rows = curses_get_rows();
    if (rows < 4) return;

    uint32 edit_rows = rows - 3; //row 0 = title, last 2 = status+hints 
    uint32 linenum_w = 5;
    uint32 text_x    = linenum_w + 1;
    uint32 text_w    = (cols > text_x) ? cols - text_x : 0;

    for (uint32 r = 0; r < edit_rows; r++) {
        uint32 doc_r = scroll_row + r;
        uint32 screen_r = r + 1;   //+1 for titlebar

        if (doc_r >= line_count) {
            //tilde for empty lines past EOF, like vi
            curses_set_bg(C_BG);
            curses_set_fg(C_LINENUM);
            curses_fill_rect(0, screen_r, cols, 1, ' ');
            curses_putc_at(0, screen_r, '~');
            continue;
        }

        //highlight current line
        bool is_cur = (doc_r == cur_row);
        uint32 bg = is_cur ? C_CURSOR_LINE : C_BG;

        //line number gutter
        curses_set_bg(C_BG);
        curses_set_fg(C_LINENUM);
        curses_fill_rect(0, screen_r, linenum_w, 1, ' ');
        char lnbuf[8];
        snprintf(lnbuf, sizeof(lnbuf), "%4lu", (unsigned long)(doc_r + 1));
        curses_write_at(0, screen_r, lnbuf, linenum_w);

        //separator
        curses_set_bg(C_BG);
        curses_set_fg(C_LINENUM);
        curses_putc_at(linenum_w, screen_r, ' ');

        //text
        curses_set_bg(bg);
        curses_set_fg(C_TEXT);
        curses_fill_rect(text_x, screen_r, text_w, 1, ' ');

        line_t *l = &lines[doc_r];
        if (l->buf && l->len > scroll_col) {
            uint32 avail = l->len - scroll_col;
            uint32 show  = (avail < text_w) ? avail : text_w;
            curses_write_at(text_x, screen_r, l->buf + scroll_col, show);
        }
    }
}

static void editor_redraw(void) {
    ensure_scroll();

    curses_set_bg(C_BG);
    curses_set_fg(C_TEXT);
    curses_clear();

    draw_titlebar();
    draw_text_area();
    draw_statusbar();
    draw_hintbar();

    //position cursor
    uint32 linenum_w = 5;
    uint32 text_x  = linenum_w + 1;
    uint32 screen_col = text_x + (cur_col >= scroll_col ? cur_col - scroll_col : 0);
    uint32 screen_row = 1 + (cur_row >= scroll_row ? cur_row - scroll_row : 0);
    curses_set_cursor(screen_col, screen_row);
    curses_show_cursor(true);

    curses_flush();
}

//prompt widget
//returns 0 on submit, -1 on cancel/esc
static int editor_prompt(const char *prompt, char *out, uint32 bufsz) {
    uint32 cols = curses_get_cols();
    uint32 rows = curses_get_rows();
    if (rows < 2 || cols < 4) return -1;

    uint32 py = rows - 2;
    uint32 plabel = (uint32)strlen(prompt);
    uint32 px = (plabel + 2 < cols) ? plabel + 2 : 0;
    uint32 pwidth = (cols > px + 2) ? cols - px - 2 : 1;

    uint32 len = 0;
    out[0] = '\0';

    for (;;) {
        //draw prompt row
        curses_set_bg(C_PROMPT_BG);
        curses_set_fg(C_HINT_KEY);
        curses_fill_rect(0, py, cols, 1, ' ');
        curses_write_at(0, py, prompt, plabel);

        curses_set_bg(C_PROMPT_BG);
        curses_set_fg(C_PROMPT_FG);
        curses_write_at(plabel + 1, py, out, pwidth);
        curses_set_cursor(px + len, py);
        curses_show_cursor(true);
        curses_flush();

        kbd_event_t ev;
        if (curses_read(&ev) < 0) return -1;
        if (!ev.pressed) continue;

        if (ev.codepoint == 0x1B) return -1;

        if (curses_event_is_submit(&ev)) return 0;

        if (curses_event_is_backspace(&ev)) {
            if (len > 0) { len--; out[len] = '\0'; }
            continue;
        }

        if (curses_event_is_printable(&ev) && len + 1 < bufsz) {
            out[len++] = curses_event_char(&ev);
            out[len]   = '\0';
        }
    }
}

//file I/O
static void doc_clear(void) {
    for (uint32 i = 0; i < line_count; i++) line_free(&lines[i]);
    doc_ensure_cap(1);
    line_count = 1;
    line_init(&lines[0], "", 0);
    cur_row = cur_col = scroll_row = scroll_col = preferred_col = 0;
    modified = false;
}

static void editor_load(const char *path) {
    handle_t fh = get_obj(INVALID_HANDLE, path, RIGHT_READ);
    if (fh == INVALID_HANDLE) {
        //file doesn't exist yet - set filename and start blan
        strncpy(filename, path, FILENAME_MAX - 1);
        filename[FILENAME_MAX - 1] = '\0';
        editor_status("New file");
        return;
    }

    doc_clear();

    char rbuf[512];
    int got;
    uint32 cur_line = 0;

    line_free(&lines[0]);
    line_init(&lines[0], "", 0);

    while ((got = handle_read(fh, rbuf, (int)sizeof(rbuf))) > 0) {
        for (int i = 0; i < got; i++) {
            char c = rbuf[i];
            if (c == '\n') {
                cur_line++;
                doc_ensure_cap(cur_line + 1);
                if (cur_line >= line_count) {
                    line_init(&lines[cur_line], "", 0);
                    line_count = cur_line + 1;
                }
            } else if (c != '\r') {
                line_append(&lines[cur_line], &c, 1);
            }
        }
    }

    handle_close(fh);
    strncpy(filename, path, FILENAME_MAX - 1);
    filename[FILENAME_MAX - 1] = '\0';
    cur_row = cur_col = scroll_row = scroll_col = 0;
    modified = false;
    editor_status("File loaded");
}

static void editor_save(void) {
    if (!filename[0]) {
        char tmp[FILENAME_MAX] = "";
        if (editor_prompt("Save as: ", tmp, FILENAME_MAX) != 0 || !tmp[0]) {
            editor_status("Save cancelled");
            editor_redraw();
            return;
        }
        strncpy(filename, tmp, FILENAME_MAX - 1);
        filename[FILENAME_MAX - 1] = '\0';
    }

    //create the file if it doesn't exist yet
    stat_t st;
    if (stat(filename, &st) < 0) {
        if (mkfile(filename) < 0) {
            editor_status("Cannot create file");
            editor_redraw();
            return;
        }
    }

    handle_t fh = get_obj(INVALID_HANDLE, filename, RIGHT_WRITE);
    if (fh == INVALID_HANDLE) {
        editor_status("Cannot write file");
        editor_redraw();
        return;
    }

    handle_seek(fh, 0, HANDLE_SEEK_SET);

    for (uint32 i = 0; i < line_count; i++) {
        line_t *l = &lines[i];
        if (l->buf && l->len > 0)
            handle_write(fh, l->buf, (int)l->len);
        if (i + 1 < line_count)
            handle_write(fh, "\n", 1);
    }

    handle_close(fh);
    modified = false;
    editor_status("Saved");
}

//edit operations
static void op_insert_char(char c) {
    line_t *l = &lines[cur_row];
    line_insert(l, cur_col, c);
    cur_col++;
    preferred_col = cur_col;
    modified = true;
}

static void op_enter(void) {
    line_t *l = &lines[cur_row];
    //split current line at cursor
    uint32 tail_len = l->len - cur_col;
    char *tail = NULL;
    if (tail_len > 0) {
        tail = malloc(tail_len + 1);
        if (tail) {
            memcpy(tail, l->buf + cur_col, tail_len);
            tail[tail_len] = '\0';
            l->len = cur_col;
            if (l->buf) l->buf[l->len] = '\0';
        }
    }
    doc_insert_line_after(cur_row);
    cur_row++;
    if (tail) {
        line_t *nl = &lines[cur_row];
        line_append(nl, tail, tail_len);
        free(tail);
    }
    cur_col = 0;
    preferred_col = 0;
    modified = true;
}

static void op_backspace(void) {
    if (cur_col > 0) {
        cur_col--;
        line_delete(&lines[cur_row], cur_col);
        preferred_col = cur_col;
        modified = true;
    } else if (cur_row > 0) {
        //merge with previous line
        line_t *prev = &lines[cur_row - 1];
        line_t *cur  = &lines[cur_row];
        uint32 prev_end = prev->len;
        if (cur->buf && cur->len > 0)
            line_append(prev, cur->buf, cur->len);
        cur_row--;
        cur_col = prev_end;
        preferred_col = cur_col;
        doc_remove_line(cur_row + 1);
        modified = true;
    }
}

static void op_delete(void) {
    line_t *l = &lines[cur_row];
    if (cur_col < l->len) {
        line_delete(l, cur_col);
        modified = true;
    } else if (cur_row + 1 < line_count) {
        //merge next line into current
        line_t *next = &lines[cur_row + 1];
        if (next->buf && next->len > 0) {
            line_append(l, next->buf, next->len);
        }
        doc_remove_line(cur_row + 1);
        modified = true;
    }
}

static void op_cut_line(void) {
    line_t *l = &lines[cur_row];
    if (l->buf && l->len > 0) {
        uint32 copy = l->len < CLIP_MAX - 1 ? l->len : CLIP_MAX - 1;
        memcpy(clipboard, l->buf, copy);
        clipboard[copy] = '\0';
        clip_len = copy;
    } else {
        clipboard[0] = '\0';
        clip_len = 0;
    }
    if (line_count > 1) {
        doc_remove_line(cur_row);
        if (cur_row >= line_count) cur_row = line_count - 1;
    } else {
        l->len = 0;
        if (l->buf) l->buf[0] = '\0';
    }
    cur_col = 0;
    preferred_col = 0;
    modified = true;
    editor_status("Line cut");
}

static void op_paste(void) {
    if (!clip_len) { editor_status("Clipboard empty"); return; }
    //insert clipboard text as a new line before current
    doc_insert_line_after(cur_row == 0 ? 0 : cur_row - 1);
    if (cur_row == 0) {
        //special: paste before first lin 
        line_t tmp = lines[0];
        lines[0] = lines[1];
        lines[1] = tmp;
    } else {
        cur_row++;
    }
    line_t *nl = &lines[cur_row];
    line_append(nl, clipboard, clip_len);
    cur_col = 0;
    preferred_col = 0;
    modified = true;
    editor_status("Pasted");
}

//navigation
static void move_up(void) {
    if (cur_row == 0) return;
    cur_row--;
    uint32 pc = preferred_col;
    clamp_cursor();
    if (lines[cur_row].len >= pc) { 
        cur_col = pc;
    } 
    else {
        cur_col = lines[cur_row].len;
    }
}

static void move_down(void) {
    if (cur_row + 1 >= line_count) return;
    cur_row++;
    uint32 pc = preferred_col;
    clamp_cursor();
    if (lines[cur_row].len >= pc) { 
        cur_col = pc;
    } 
    else cur_col = lines[cur_row].len;
}

static void move_left(void) {
    if (cur_col > 0) { 
        cur_col--; 
    }
    else if (cur_row > 0) { 
        cur_row--; 
        cur_col = lines[cur_row].len; 
    }
    preferred_col = cur_col;
}

static void move_right(void) {
    if (cur_col < lines[cur_row].len) { 
        cur_col++; 
    }
    else if (cur_row + 1 < line_count) { 
        cur_row++; 
        cur_col = 0; 
    }
    preferred_col = cur_col;
}

static void move_home(void) {
    cur_col = 0; preferred_col = 0;
}

static void move_end(void) {
    cur_col = lines[cur_row].len; preferred_col = cur_col;
}

static void move_pgup(void) {
    uint32 rows = curses_get_rows();
    uint32 page = (rows > 4) ? rows - 3 : 1;
    cur_row = (cur_row >= page) ? cur_row - page : 0;
    clamp_cursor();
    preferred_col = cur_col;
}

static void move_pgdn(void) {
    uint32 rows = curses_get_rows();
    uint32 page = (rows > 4) ? rows - 3 : 1;
    cur_row += page;
    if (cur_row >= line_count) cur_row = line_count - 1;
    clamp_cursor();
    preferred_col = cur_col;
}

static void op_goto_line(void) {
    char buf[16] = "";
    if (editor_prompt("Go to line: ", buf, sizeof(buf)) != 0) {
        editor_redraw();
        return;
    }
    uint32 target = 0;
    for (uint32 i = 0; buf[i]; i++) {
        if (buf[i] < '0' || buf[i] > '9') { 
            editor_status("Invalid line number"); 
            editor_redraw(); 
            return; 
        }
        target = target * 10 + (uint32)(buf[i] - '0');
    }
    if (target == 0) target = 1;
    if (target > line_count) target = line_count;
    cur_row = target - 1;
    cur_col = 0;
    preferred_col = 0;
    editor_redraw();
}

//exit / open helpers
static bool confirm_discard(void) {
    if (!modified) return true;
    char ans[4] = "";
    int r = editor_prompt("Unsaved changes! Discard? (y/n): ", ans, sizeof(ans));
    editor_redraw();
    if (r != 0) return false;
    return (ans[0] == 'y' || ans[0] == 'Y');
}

static void op_open(void) {
    char path[FILENAME_MAX] = "";
    if (editor_prompt("Open file: ", path, FILENAME_MAX) != 0 || !path[0]) {
        editor_status("Open cancelled");
        editor_redraw();
        return;
    }
    if (modified && !confirm_discard()) return;
    editor_load(path);
    editor_redraw();
}

//main
int main(int argc, char **argv) {
    if (curses_init() < 0) {
        puts("editor: failed to initialize curses\n");
        return 1;
    }

    //initialise document with one blank line
    doc_clear();

    //load file from argument if given 
    if (argc >= 2 && argv[1] && argv[1][0]) {
        editor_load(argv[1]);
    }

    editor_redraw();

    while (1) {
        kbd_event_t ev;
        if (curses_read(&ev) < 0) break;
        if (!ev.pressed) continue;

        uint32 cp = ev.codepoint;
        uint8  mods = ev.mods;
        bool   ctrl = (mods & KBD_MOD_CTRL) != 0;

        //ctrl shortcuts 
        if (ctrl) {
            switch (cp) {
            case 'x': case 'X':
                if (!confirm_discard()) { 
                    editor_redraw(); 
                    break; 
                }
                curses_shutdown();
                return 0;

            case 's': case 'S':
                editor_save();
                editor_redraw();
                break;

            case 'o': case 'O':
                op_open();
                break;

            case 'k': case 'K':
                op_cut_line();
                editor_redraw();
                break;

            case 'u': case 'U':
                op_paste();
                editor_redraw();
                break;

            case 'g': case 'G':
                op_goto_line();
                break;

            default:
                break;
            }
            continue;
        }

        //extended / arrow keys
        if ((cp & 0xFF00u) == 0xE000u) {
            switch (cp) {
            case KEY_UP: move_up(); break;
            case KEY_DOWN: move_down(); break;
            case KEY_LEFT: move_left(); break;
            case KEY_RIGHT: move_right(); break;
            case KEY_HOME: move_home(); break;
            case KEY_END: move_end(); break;
            case KEY_PGUP: move_pgup(); break;
            case KEY_PGDN: move_pgdn(); break;
            case KEY_DEL: op_delete(); break;
            default: break;
            }
            editor_redraw();
            continue;
        }

        //regular printable characters
        if (curses_event_is_submit(&ev)) {
            op_enter();
            editor_redraw();
            continue;
        }

        if (curses_event_is_backspace(&ev)) {
            op_backspace();
            editor_redraw();
            continue;
        }

        if (curses_event_is_printable(&ev)) {
            char c = curses_event_char(&ev);
            if (c) {
                op_insert_char(c);
                editor_redraw();
            }
            continue;
        }
    }

    curses_shutdown();
    return 0;
}
