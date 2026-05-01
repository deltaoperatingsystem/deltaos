#include "vt.h"
#include <drivers/console.h>
#include <drivers/fb.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <arch/cpu.h>
#include <syscall/syscall.h>
#include <string.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <drivers/init.h>
#include <arch/timer.h>

//active VT index
static int active_vt = 0;

//array of VTs
static vt_t *vts[VT_MAX] = {0};
static spinlock_irq_t vt_lock = SPINLOCK_IRQ_INIT;
static vt_t *const VT_RESERVED_SLOT = (vt_t *)-1;

//default colors
#define DEFAULT_FG  0xFFFFFF
#define DEFAULT_BG  0x000000
#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define VT_CURSOR_UNDERLINE_HEIGHT 2
#define VT_CURSOR_BLINK_HZ 2

//mark a row as needing redraw
static inline void mark_dirty(vt_t *vt, uint32 row) {
    if (row < vt->dirty_start) vt->dirty_start = row;
    if (row > vt->dirty_end) vt->dirty_end = row;
}

static void vt_render_to_console_locked(vt_t *vt);
static bool vt_cursor_should_be_visible(void);
static void vt_draw_cell(vt_t *vt, uint32 col, uint32 row, bool cursor_visible);
static void vt_hide_cursor_locked(vt_t *vt);
static void vt_sync_cursor_locked(vt_t *vt, bool flip);
static void vt_clear_locked(vt_t *vt);
static void vt_set_cursor_locked(vt_t *vt, uint32 col, uint32 row);
static void vt_set_cursor_visible_locked(vt_t *vt, bool visible);
static void vt_set_attr_locked(vt_t *vt, int attr, uint32 value);
static void vt_flush_locked(vt_t *vt);
static void vt_write_locked(vt_t *vt, const char *s, size len);
static void vt_write_cells_locked(vt_t *vt, uint32 col, uint32 row, const vt_cell_t *cells, size count);
static bool vt_poll_event_locked(vt_t *vt, vt_event_t *event);
static void vt_push_event_locked(vt_t *vt, const vt_event_t *event);
static void vt_putc_locked(vt_t *vt, char c);
static vt_t *vt_get_locked(int num);
static vt_t *vt_get_active_locked(void);
vt_t *vt_create(void);
void vt_destroy(vt_t *vt);
vt_t *vt_get(int num);
vt_t *vt_get_active(void);

static void vt_scroll(vt_t *vt) {
    vt_hide_cursor_locked(vt);

    //flush any pending dirty rows before scrolling
    //otherwise content written to buffer but not rendered will be lost
    if (vt->vt_num == active_vt && vt->dirty_start <= vt->dirty_end) {
        vt_render_to_console_locked(vt);
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
    }
    
    //reset dirty tracking
    vt->dirty_start = vt->rows - 1;
    vt->dirty_end = vt->rows - 1;
}

static void vt_newline(vt_t *vt) {
    vt_hide_cursor_locked(vt);
    vt->cursor_col = 0;
    vt->cursor_row++;
    if (vt->cursor_row >= vt->rows) {
        vt_scroll(vt);
        vt->cursor_row = vt->rows - 1;
    }
}

static bool vt_cursor_should_be_visible(void) {
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) return true;

    uint64 phase_ticks = (uint64)freq / VT_CURSOR_BLINK_HZ;
    if (phase_ticks == 0) phase_ticks = 1;

    return ((arch_timer_get_ticks() / phase_ticks) & 1U) == 0;
}

static void vt_draw_cell(vt_t *vt, uint32 col, uint32 row, bool cursor_visible) {
    if (!vt || !fb_available() || col >= vt->cols || row >= vt->rows) return;

    vt_cell_t *cell = &vt->cells[row * vt->cols + col];
    con_draw_char_at(col, row, (char)cell->codepoint, cell->fg, cell->bg);

    if (cursor_visible) {
        uint32 x = col * FONT_WIDTH;
        uint32 y = row * FONT_HEIGHT + FONT_HEIGHT - VT_CURSOR_UNDERLINE_HEIGHT;
        fb_fillrect(x, y, FONT_WIDTH, VT_CURSOR_UNDERLINE_HEIGHT, cell->fg);
    }
}

static void vt_hide_cursor_locked(vt_t *vt) {
    if (!vt || vt->vt_num != active_vt || !vt->cursor_drawn) return;

    vt_draw_cell(vt, vt->cursor_drawn_col, vt->cursor_drawn_row, false);
    vt->cursor_drawn = false;
}

static void vt_sync_cursor_locked(vt_t *vt, bool flip) {
    if (!vt || vt->vt_num != active_vt || !fb_available()) return;

    bool want_visible = vt->cursor_enabled && vt_cursor_should_be_visible();
    bool same_cell = vt->cursor_drawn
        && vt->cursor_drawn_col == vt->cursor_col
        && vt->cursor_drawn_row == vt->cursor_row;

    if (same_cell && want_visible) return;

    if (vt->cursor_drawn) {
        vt_draw_cell(vt, vt->cursor_drawn_col, vt->cursor_drawn_row, false);
        if (flip) {
            fb_flip_rect(vt->cursor_drawn_col * FONT_WIDTH,
                         vt->cursor_drawn_row * FONT_HEIGHT,
                         FONT_WIDTH, FONT_HEIGHT);
        }
        vt->cursor_drawn = false;
    }

    if (!want_visible || vt->cursor_col >= vt->cols || vt->cursor_row >= vt->rows) return;

    vt_draw_cell(vt, vt->cursor_col, vt->cursor_row, true);
    vt->cursor_drawn = true;
    vt->cursor_drawn_col = vt->cursor_col;
    vt->cursor_drawn_row = vt->cursor_row;

    if (flip) {
        fb_flip_rect(vt->cursor_col * FONT_WIDTH,
                     vt->cursor_row * FONT_HEIGHT,
                     FONT_WIDTH, FONT_HEIGHT);
    }
}

static void vt_render_to_console_locked(vt_t *vt) {
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

    vt->cursor_drawn = false;
    vt_sync_cursor_locked(vt, false);
    con_flush();
}

static ssize vt_obj_read(object_t *obj, void *buf, size len, size offset) {
    (void)offset;
    vt_t *vt = (vt_t *)obj->data;
    if (!vt) return 0;
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    //read events as raw bytes
    vt_event_t *out = buf;
    size count = 0;
    size max_events = len / sizeof(vt_event_t);

    while (count < max_events && vt->event_head != vt->event_tail) {
        out[count++] = vt->events[vt->event_tail];
        vt->event_tail = (vt->event_tail + 1) % VT_EVENT_BUFSIZE;
    }

    spinlock_irq_release(&vt_lock, flags);
    return count * sizeof(vt_event_t);
}

static ssize vt_obj_write(object_t *obj, const void *buf, size len, size offset) {
    (void)offset;
    vt_t *vt = (vt_t *)obj->data;
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_write_locked(vt, buf, len);
    vt_flush_locked(vt);  //flush to screen immediately
    spinlock_irq_release(&vt_lock, flags);
    return len;
}

static intptr vt_obj_get_info(object_t *obj, uint32 topic, void *buf, size len) {
    vt_t *vt = (vt_t *)obj->data;
    if (!vt || !buf) return -1;

    if (topic == OBJ_INFO_VT_STATE) {
        if (len < sizeof(vt_info_t)) return -1;
        irq_state_t flags = spinlock_irq_acquire(&vt_lock);
        vt_info_t info;
        info.cols = vt->cols;
        info.rows = vt->rows;
        info.cursor_col = vt->cursor_col;
        info.cursor_row = vt->cursor_row;
        spinlock_irq_release(&vt_lock, flags);
        memcpy(buf, &info, sizeof(info));
        return 0;
    }

    return -1;
}

static object_ops_t vt_object_ops = {
    .read = vt_obj_read,
    .write = vt_obj_write,
    .close = NULL,
    .get_info = vt_obj_get_info
};

void vt_init(void) {
    if (vt_get(0)) return;

    //create VT0 by default
    vt_t *vt0 = vt_create();
    if (vt0) {
        active_vt = 0;
    }
}

DECLARE_DRIVER(vt_init, INIT_LEVEL_FS);

vt_t *vt_create(void) {
    //find and reserve a free slot before any sleeping allocations
    int num = -1;
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    for (int i = 0; i < VT_MAX; i++) {
        if (!vts[i]) {
            num = i;
            vts[i] = VT_RESERVED_SLOT;
            break;
        }
    }
    spinlock_irq_release(&vt_lock, flags);
    if (num < 0) return NULL;
    
    vt_t *vt = kzalloc(sizeof(vt_t));
    if (!vt) {
        flags = spinlock_irq_acquire(&vt_lock);
        if (vts[num] == VT_RESERVED_SLOT) vts[num] = NULL;
        spinlock_irq_release(&vt_lock, flags);
        return NULL;
    }
    
    vt->cols = con_cols();
    vt->rows = con_rows();
    vt->cells = kzalloc(vt->cols * vt->rows * sizeof(vt_cell_t));
    if (!vt->cells) {
        flags = spinlock_irq_acquire(&vt_lock);
        if (vts[num] == VT_RESERVED_SLOT) vts[num] = NULL;
        spinlock_irq_release(&vt_lock, flags);
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
    vt->cursor_enabled = true;
    vt->cursor_drawn = false;
    vt->cursor_drawn_col = 0;
    vt->cursor_drawn_row = 0;
    vt->event_head = 0;
    vt->event_tail = 0;
    vt->vt_num = num;
    
    //start with nothing dirty (blank screen)
    vt->dirty_start = vt->rows;
    vt->dirty_end = 0;

    flags = spinlock_irq_acquire(&vt_lock);
    if (vts[num] != VT_RESERVED_SLOT) {
        spinlock_irq_release(&vt_lock, flags);
        kfree(vt->cells);
        kfree(vt);
        return NULL;
    }
    vts[num] = vt;
    spinlock_irq_release(&vt_lock, flags);

    //create object and register after the slot has been claimed
    vt->obj = object_create(OBJECT_DEVICE, &vt_object_ops, vt);
    if (!vt->obj) {
        vt_destroy(vt);
        return NULL;
    }

    char path[32];
    snprintf(path, sizeof(path), "$devices/vt%d", num);
    if (ns_register(path, vt->obj) < 0) {
        object_deref(vt->obj);
        vt->obj = NULL;
        vt_destroy(vt);
        return NULL;
    }

    return vt;
}

void vt_destroy(vt_t *vt) {
    if (!vt) return;

    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    if (vt->vt_num >= 0 && vt->vt_num < VT_MAX && vts[vt->vt_num] == vt) {
        vts[vt->vt_num] = NULL;
    }
    if (vt->vt_num == active_vt) {
        active_vt = 0;
    }
    spinlock_irq_release(&vt_lock, flags);
    
    if (vt->cells) kfree(vt->cells);
    kfree(vt);
}

vt_t *vt_get(int num) {
    if (num < 0 || num >= VT_MAX) return NULL;
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_t *vt = vt_get_locked(num);
    spinlock_irq_release(&vt_lock, flags);
    return vt;
}

vt_t *vt_get_active(void) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_t *vt = vt_get_active_locked();
    spinlock_irq_release(&vt_lock, flags);
    return vt;
}

void vt_switch(int num) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_t *new_vt = vt_get_locked(num);
    if (!new_vt) {
        spinlock_irq_release(&vt_lock, flags);
        return;
    }

    vt_t *old = vt_get_active_locked();
    if (old) vt_hide_cursor_locked(old);
    active_vt = num;
    new_vt->dirty_start = 0;
    new_vt->dirty_end = new_vt->rows - 1;
    vt_render_to_console_locked(new_vt);
    spinlock_irq_release(&vt_lock, flags);
}

static bool vt_poll_event_locked(vt_t *vt, vt_event_t *event) {
    if (!vt || vt->event_head == vt->event_tail) return false;

    *event = vt->events[vt->event_tail];
    vt->event_tail = (vt->event_tail + 1) % VT_EVENT_BUFSIZE;
    return true;
}

bool vt_poll_event(vt_t *vt, vt_event_t *event) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    bool ok = vt_poll_event_locked(vt, event);
    spinlock_irq_release(&vt_lock, flags);
    return ok;
}

void vt_wait_event(vt_t *vt, vt_event_t *event) {
    while (!vt_poll_event(vt, event)) {
        arch_halt();
    }
}

static void vt_push_event_locked(vt_t *vt, const vt_event_t *event) {
    if (!vt) return;

    uint8 next = (vt->event_head + 1) % VT_EVENT_BUFSIZE;
    if (next != vt->event_tail) {
        vt->events[vt->event_head] = *event;
        vt->event_head = next;
    }
}

void vt_push_event(vt_t *vt, const vt_event_t *event) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_push_event_locked(vt, event);
    spinlock_irq_release(&vt_lock, flags);
}

static void vt_set_attr_locked(vt_t *vt, int attr, uint32 value) {
    if (!vt) return;

    switch (attr) {
        case VT_ATTR_FG: vt->fg_color = value; break;
        case VT_ATTR_BG: vt->bg_color = value; break;
        default: break;
    }
}

void vt_set_attr(vt_t *vt, int attr, uint32 value) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_set_attr_locked(vt, attr, value);
    spinlock_irq_release(&vt_lock, flags);
}

static void vt_putc_locked(vt_t *vt, char c) {
    if (!vt) return;
    
    if (c == '\n') {
        mark_dirty(vt, vt->cursor_row);
        vt_newline(vt);
    } else if (c == '\f') {
        vt_clear_locked(vt);
    } else if (c == '\r') {
        vt_hide_cursor_locked(vt);
        vt->cursor_col = 0;
    } else if (c == '\t') {
        mark_dirty(vt, vt->cursor_row);
        vt_hide_cursor_locked(vt);
        vt->cursor_col = (vt->cursor_col + 4) & ~3;
        if (vt->cursor_col >= vt->cols) vt_newline(vt);
    } else if (c == '\b') {
        if (vt->cursor_col > 0) {
            vt_hide_cursor_locked(vt);
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
        
        vt_hide_cursor_locked(vt);
        vt->cursor_col++;
        if (vt->cursor_col >= vt->cols) {
            vt->cursor_col = vt->cols - 1;
        }
    }
}

void vt_putc(vt_t *vt, char c) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_putc_locked(vt, c);
    spinlock_irq_release(&vt_lock, flags);
}

static void vt_write_locked(vt_t *vt, const char *s, size len) {
    if (!vt || !s) return;
    for (size i = 0; i < len; i++) {
        if (s[i] == '\033') {
            if (i + 1 >= len) break;

            char mode = s[++i];
            if (mode == 'H') {
                vt_set_cursor_locked(vt, 0, 0);
                continue;
            }

            if (mode == 'P') {
                if (i + 8 >= len) continue;

                uint32 row = 0;
                uint32 col = 0;
                for (int shift = 12; shift >= 0; shift -= 4) {
                    row |= (uint32)ctoh(s[++i]) << shift;
                }
                for (int shift = 12; shift >= 0; shift -= 4) {
                    col |= (uint32)ctoh(s[++i]) << shift;
                }

                vt_set_cursor_locked(vt, col, row);
                continue;
            }

            if (mode == 'v') {
                if (i + 1 >= len) continue;
                char state = s[++i];
                if (state == '0') vt_set_cursor_visible_locked(vt, false);
                else if (state == '1') vt_set_cursor_visible_locked(vt, true);
                continue;
            }

            if (mode == 'f' || mode == 'b') {
                // SET COLOUR
                // args: 0xNNNNNN
                if (i + 6 >= len) continue;  //need 6 hex digits after the mode

                uint32 colour = 0;
                colour |= (uint32)ctoh(s[++i]) << 20;
                colour |= (uint32)ctoh(s[++i]) << 16;
                colour |= (uint32)ctoh(s[++i]) << 12;
                colour |= (uint32)ctoh(s[++i]) << 8;
                colour |= (uint32)ctoh(s[++i]) << 4;
                colour |= (uint32)ctoh(s[++i]);

                if (mode == 'f') vt->fg_color = colour;
                else vt->bg_color = colour;
            }

            continue;
        }

        vt_putc_locked(vt, s[i]);
    }
}

void vt_write(vt_t *vt, const char *s, size len) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_write_locked(vt, s, len);
    spinlock_irq_release(&vt_lock, flags);
}

void vt_print(vt_t *vt, const char *s) {
    if (!vt || !s) return;
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    while (*s) {
        vt_putc_locked(vt, *s++);
    }
    spinlock_irq_release(&vt_lock, flags);
}

static void vt_clear_locked(vt_t *vt) {
    if (!vt) return;

    vt_hide_cursor_locked(vt);
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

void vt_clear(vt_t *vt) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_clear_locked(vt);
    spinlock_irq_release(&vt_lock, flags);
}

static void vt_set_cursor_locked(vt_t *vt, uint32 col, uint32 row) {
    if (!vt) return;

    vt_hide_cursor_locked(vt);
    if (col < vt->cols) vt->cursor_col = col;
    if (row < vt->rows) vt->cursor_row = row;
}

void vt_set_cursor(vt_t *vt, uint32 col, uint32 row) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_set_cursor_locked(vt, col, row);
    spinlock_irq_release(&vt_lock, flags);
}

static void vt_set_cursor_visible_locked(vt_t *vt, bool visible) {
    if (!vt) return;
    vt->cursor_enabled = visible;
    if (!visible) {
        vt_hide_cursor_locked(vt);
        if (vt->vt_num == active_vt) {
            con_flush();
        }
        return;
    }

    if (vt->vt_num == active_vt) {
        vt_sync_cursor_locked(vt, false);
        con_flush();
    }
}

void vt_set_cursor_visible(vt_t *vt, bool visible) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_set_cursor_visible_locked(vt, visible);
    spinlock_irq_release(&vt_lock, flags);
}

uint32 vt_cols(vt_t *vt) {
    return vt ? vt->cols : 0;
}

uint32 vt_rows(vt_t *vt) {
    return vt ? vt->rows : 0;
}

static void vt_flush_locked(vt_t *vt) {
    if (!vt) return;
    if (vt->vt_num == active_vt) {
        vt_render_to_console_locked(vt);
    }
}

void vt_flush(vt_t *vt) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_flush_locked(vt);
    spinlock_irq_release(&vt_lock, flags);
}

static void vt_write_cells_locked(vt_t *vt, uint32 col, uint32 row, const vt_cell_t *cells, size count) {
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

void vt_write_cells(vt_t *vt, uint32 col, uint32 row, const vt_cell_t *cells, size count) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_write_cells_locked(vt, col, row, cells, count);
    spinlock_irq_release(&vt_lock, flags);
}

void vt_tick(void) {
    irq_state_t flags = spinlock_irq_acquire(&vt_lock);
    vt_t *vt = vt_get_active_locked();
    if (!vt || !fb_available()) {
        spinlock_irq_release(&vt_lock, flags);
        return;
    }

    vt_sync_cursor_locked(vt, true);
    spinlock_irq_release(&vt_lock, flags);
}

static vt_t *vt_get_locked(int num) {
    if (num < 0 || num >= VT_MAX) return NULL;
    if (vts[num] == VT_RESERVED_SLOT) return NULL;
    return vts[num];
}

static vt_t *vt_get_active_locked(void) {
    if (active_vt < 0 || active_vt >= VT_MAX) return NULL;
    return vt_get_locked(active_vt);
}
