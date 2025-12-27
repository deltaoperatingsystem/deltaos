#ifndef DRIVERS_VT_H
#define DRIVERS_VT_H

#include <arch/types.h>

//maximum number of virtual terminals
#define VT_MAX          8

//event buffer size per VT
#define VT_EVENT_BUFSIZE 256

//input event types
#define VT_EVENT_KEY    1   //key press/release
#define VT_EVENT_MOUSE  2   //future mouse support

//modifier flags
#define VT_MOD_SHIFT    (1 << 0)
#define VT_MOD_CTRL     (1 << 1)
#define VT_MOD_ALT      (1 << 2)
#define VT_MOD_CAPSLOCK (1 << 3)

//key event structure
typedef struct {
    uint8   type;       //VT_EVENT_*
    uint8   mods;       //modifier flags (VT_MOD_*)
    uint16  keycode;    //raw scancode
    uint32  codepoint;  //unicode codepoint (ASCII-compatible for 0-127)
    bool    pressed;    //true = press false = release
} vt_event_t;

//text attribute types for vt_set_attr
#define VT_ATTR_FG      1   //foreground color (uint32 RGB)
#define VT_ATTR_BG      2   //background color (uint32 RGB)
#define VT_ATTR_BOLD    3   //bold flag (0 or 1)

//character cell in VT buffer
typedef struct {
    uint32  codepoint;  //unicode codepoint (ASCII-compatible for 0-127)
    uint32  fg;
    uint32  bg;
} vt_cell_t;

//VT structure
typedef struct vt {
    //input event queue
    vt_event_t events[VT_EVENT_BUFSIZE];
    uint16 event_head;
    uint16 event_tail;
    
    //output buffer
    vt_cell_t *cells;       //cols * rows cells
    uint32 cols;
    uint32 rows;
    
    //cursor state
    uint32 cursor_col;
    uint32 cursor_row;
    
    //current attributes
    uint32 fg_color;
    uint32 bg_color;
    
    //dirty tracking (row range that needs redraw)
    uint32 dirty_start;     //first dirty row
    uint32 dirty_end;       //last dirty row
    
    //VT number (0-based)
    int vt_num;
    
    //object for namespace
    struct object *obj;
} vt_t;

//initialize VT subsystem
void vt_init(void);

//create a new VT returns NULL on failure
vt_t *vt_create(void);

//destroy a VT
void vt_destroy(vt_t *vt);

//get VT by number (0 to VT_MAX-1)
vt_t *vt_get(int num);

//get currently active VT
vt_t *vt_get_active(void);

//switch to a different VT (redraws screen)
void vt_switch(int num);

//poll for an event (non-blocking)
//returns true if event available andd false otherwise
bool vt_poll_event(vt_t *vt, vt_event_t *event);

//wait for an event (blocking)
void vt_wait_event(vt_t *vt, vt_event_t *event);

//push an event to a VT's queue (called by keyboard driver)
void vt_push_event(vt_t *vt, const vt_event_t *event);

//set text attribute
void vt_set_attr(vt_t *vt, int attr, uint32 value);

//write string to VT at cursor
void vt_write(vt_t *vt, const char *s, size len);

//put single character at cursor
void vt_putc(vt_t *vt, char c);

//print null-terminated string
void vt_print(vt_t *vt, const char *s);

//clear screen
void vt_clear(vt_t *vt);

//set cursor position
void vt_set_cursor(vt_t *vt, uint32 col, uint32 row);

//get dimensions
uint32 vt_cols(vt_t *vt);
uint32 vt_rows(vt_t *vt);

//flush VT to screen (only if this VT is active)
void vt_flush(vt_t *vt);

//write an array of cells directly to VT at position
void vt_write_cells(vt_t *vt, uint32 col, uint32 row, const vt_cell_t *cells, size count);

#endif
