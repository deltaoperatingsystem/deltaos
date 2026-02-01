#include "vt.h"
#include <drivers/console.h>
#include <drivers/fb.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <arch/cpu.h>
#include <string.h>
#include <lib/io.h>

//active VT index
static int active_vt = 0;

//array of VTs
static vt_t *vts[VT_MAX] = {0};

//default colors
#define DEFAULT_FG  0xFFFFFF
#define DEFAULT_BG  0x000000

//mark a row as needing redraw
static inline void mark_dirty(vt_t *vt, uint32 row) {
    if (row < vt->dirty_start) vt->dirty_start = row;
    if (row > vt->dirty_end) vt->dirty_end = row;
}

#define FONT_HEIGHT 16

static void vt_render_to_console(vt_t *vt);

static void vt_scroll(vt_t *vt) {
    //flush any pending dirty rows before scrolling
    //otherwise content written to buffer but not rendered will be lost
    if (vt->vt_num == active_vt && vt->dirty_start <= vt->dirty_end) {
        vt_render_to_console(vt);
    }
    
    //move VT buffer rows up by one
    size row_size = vt->cols * sizeof(vt_cell_t);
    memmove(vt->cells, vt->cells + vt->cols, row_size * (vt->rows - 1));
    
    //clear last row in buffer
    vt_cell_t *last_row = vt->cells + (vt->rows - 1) * vt->cols;
    for (uint32 i = 0; i < vt->cols; i++) {
        last_row[i].codepoint = ' ';
        last_row[i].fg = vt->fg_color;
        last_row[i].bg = vt->bg_color;
    }
    
    //scroll the framebuffer (if this VT is active)
    if (vt->vt_num == active_vt && fb_available()) {
        fb_scroll(FONT_HEIGHT, vt->bg_color);
        fb_flip();  //commit the scroll to screen immediately
    }
    
    //reset dirty tracking
    vt->dirty_start = vt->rows - 1;
    vt->dirty_end = vt->rows - 1;
}

static void vt_newline(vt_t *vt) {
    vt->cursor_col = 0;
    vt->cursor_row++;
    if (vt->cursor_row >= vt->rows) {
        vt_scroll(vt);
        vt->cursor_row = vt->rows - 1;
    }
}

static void vt_render_to_console(vt_t *vt) {
    if (!fb_available()) return;
    
    //only render rows that have been touched since last flush
    uint32 start_row = vt->dirty_start;
    uint32 end_row = vt->dirty_end;
    
    if (start_row > end_row) return;  //nothing dirty
    
    for (uint32 row = start_row; row <= end_row && row < vt->rows; row++) {
        for (uint32 col = 0; col < vt->cols; col++) {
            vt_cell_t *cell = &vt->cells[row * vt->cols + col];
            //cast codepoint to char for now (BMP only until font supports UTF)
            con_draw_char_at(col, row, (char)cell->codepoint, cell->fg, cell->bg);
        }
    }
    
    //reset dirty tracking
    vt->dirty_start = vt->rows;
    vt->dirty_end = 0;
    
    con_flush();
}

static ssize vt_obj_read(object_t *obj, void *buf, size len, size offset) {
    (void)offset;
    vt_t *vt = (vt_t *)obj->data;
    
    //read events as raw bytes
    vt_event_t *out = buf;
    size count = 0;
    size max_events = len / sizeof(vt_event_t);
    
    while (count < max_events && vt->event_head != vt->event_tail) {
        out[count++] = vt->events[vt->event_tail];
        vt->event_tail = (vt->event_tail + 1) % VT_EVENT_BUFSIZE;
    }
    
    return count * sizeof(vt_event_t);
}

static ssize vt_obj_write(object_t *obj, const void *buf, size len, size offset) {
    (void)offset;
    vt_t *vt = (vt_t *)obj->data;
    vt_write(vt, buf, len);
    vt_flush(vt);  //flush to screen immediately
    return len;
}

static object_ops_t vt_object_ops = {
    .read = vt_obj_read,
    .write = vt_obj_write,
    .close = NULL
};

void vt_init(void) {
    //create VT0 by default
    vt_t *vt0 = vt_create();
    if (vt0) {
        active_vt = 0;
    }
}

vt_t *vt_create(void) {
    //find free slot
    int num = -1;
    for (int i = 0; i < VT_MAX; i++) {
        if (!vts[i]) {
            num = i;
            break;
        }
    }
    if (num < 0) return NULL;
    
    vt_t *vt = kzalloc(sizeof(vt_t));
    if (!vt) return NULL;
    
    vt->cols = con_cols();
    vt->rows = con_rows();
    vt->cells = kzalloc(vt->cols * vt->rows * sizeof(vt_cell_t));
    if (!vt->cells) {
        kfree(vt);
        return NULL;
    }
    
    //init cells
    vt->fg_color = DEFAULT_FG;
    vt->bg_color = DEFAULT_BG;
    for (uint32 i = 0; i < vt->cols * vt->rows; i++) {
        vt->cells[i].codepoint = ' ';
        vt->cells[i].fg = DEFAULT_FG;
        vt->cells[i].bg = DEFAULT_BG;
    }
    
    vt->cursor_col = 0;
    vt->cursor_row = 0;
    vt->event_head = 0;
    vt->event_tail = 0;
    vt->vt_num = num;
    
    //start with nothing dirty (blank screen)
    vt->dirty_start = vt->rows;
    vt->dirty_end = 0;
    
    //create object and register
    vt->obj = object_create(OBJECT_DEVICE, &vt_object_ops, vt);
    if (vt->obj) {
        char path[32];
        snprintf(path, sizeof(path), "$devices/vt%d", num);
        ns_register(path, vt->obj);
    }
    
    vts[num] = vt;
    return vt;
}

void vt_destroy(vt_t *vt) {
    if (!vt) return;
    
    if (vt->vt_num >= 0 && vt->vt_num < VT_MAX) {
        vts[vt->vt_num] = NULL;
    }
    
    if (vt->cells) kfree(vt->cells);
    kfree(vt);
}

vt_t *vt_get(int num) {
    if (num < 0 || num >= VT_MAX) return NULL;
    return vts[num];
}

vt_t *vt_get_active(void) {
    return vts[active_vt];
}

void vt_switch(int num) {
    if (num < 0 || num >= VT_MAX || !vts[num]) return;
    
    active_vt = num;
    vt_render_to_console(vts[num]);
}

bool vt_poll_event(vt_t *vt, vt_event_t *event) {
    if (!vt || vt->event_head == vt->event_tail) return false;
    
    *event = vt->events[vt->event_tail];
    vt->event_tail = (vt->event_tail + 1) % VT_EVENT_BUFSIZE;
    return true;
}

void vt_wait_event(vt_t *vt, vt_event_t *event) {
    while (!vt_poll_event(vt, event)) {
        arch_halt();
    }
}

void vt_push_event(vt_t *vt, const vt_event_t *event) {
    if (!vt) return;
    
    uint8 next = (vt->event_head + 1) % VT_EVENT_BUFSIZE;
    if (next != vt->event_tail) {
        vt->events[vt->event_head] = *event;
        vt->event_head = next;
    }
}

void vt_set_attr(vt_t *vt, int attr, uint32 value) {
    if (!vt) return;
    
    switch (attr) {
        case VT_ATTR_FG: vt->fg_color = value; break;
        case VT_ATTR_BG: vt->bg_color = value; break;
        default: break;
    }
}

void vt_putc(vt_t *vt, char c) {
    if (!vt) return;
    
    if (c == '\n') {
        mark_dirty(vt, vt->cursor_row);
        vt_newline(vt);
    } else if (c == '\f') {
        vt_clear(vt);
    } else if (c == '\r') {
        vt->cursor_col = 0;
    } else if (c == '\t') {
        mark_dirty(vt, vt->cursor_row);
        vt->cursor_col = (vt->cursor_col + 4) & ~3;
        if (vt->cursor_col >= vt->cols) vt_newline(vt);
    } else if (c == '\b') {
        if (vt->cursor_col > 0) {
            vt->cursor_col--;
            vt_cell_t *cell = &vt->cells[vt->cursor_row * vt->cols + vt->cursor_col];
            cell->codepoint = ' ';
            cell->fg = vt->fg_color;
            cell->bg = vt->bg_color;
            mark_dirty(vt, vt->cursor_row);
        }
    } else {
        vt_cell_t *cell = &vt->cells[vt->cursor_row * vt->cols + vt->cursor_col];
        cell->codepoint = (uint32)c;
        cell->fg = vt->fg_color;
        cell->bg = vt->bg_color;
        mark_dirty(vt, vt->cursor_row);
        
        vt->cursor_col++;
        if (vt->cursor_col >= vt->cols) {
            vt_newline(vt);
        }
    }
}

void vt_write(vt_t *vt, const char *s, size len) {
    for (size i = 0; i < len; i++) {
        vt_putc(vt, s[i]);
    }
}

void vt_print(vt_t *vt, const char *s) {
    while (*s) {
        vt_putc(vt, *s++);
    }
}

void vt_clear(vt_t *vt) {
    if (!vt) return;
    
    for (uint32 i = 0; i < vt->cols * vt->rows; i++) {
        vt->cells[i].codepoint = ' ';
        vt->cells[i].fg = vt->fg_color;
        vt->cells[i].bg = vt->bg_color;
    }
    vt->cursor_col = 0;
    vt->cursor_row = 0;
    
    //entire screen dirty
    vt->dirty_start = 0;
    vt->dirty_end = vt->rows - 1;
}

void vt_set_cursor(vt_t *vt, uint32 col, uint32 row) {
    if (!vt) return;
    if (col < vt->cols) vt->cursor_col = col;
    if (row < vt->rows) vt->cursor_row = row;
}

uint32 vt_cols(vt_t *vt) {
    return vt ? vt->cols : 0;
}

uint32 vt_rows(vt_t *vt) {
    return vt ? vt->rows : 0;
}

void vt_flush(vt_t *vt) {
    if (!vt) return;
    if (vt->vt_num == active_vt) {
        vt_render_to_console(vt);
    }
}

void vt_write_cells(vt_t *vt, uint32 col, uint32 row, const vt_cell_t *cells, size count) {
    if (!vt || !cells) return;
    
    for (size i = 0; i < count; i++) {
        uint32 c = col + i;
        uint32 r = row;
        
        //wrap to next row if needed
        while (c >= vt->cols) {
            c -= vt->cols;
            r++;
        }
        
        if (r >= vt->rows) break;  //off screen
        
        vt->cells[r * vt->cols + c] = cells[i];
        mark_dirty(vt, r);
    }
}
