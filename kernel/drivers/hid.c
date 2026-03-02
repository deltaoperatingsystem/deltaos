#include <drivers/hid.h>
#include <drivers/keyboard_protocol.h>
#include <drivers/mouse_protocol.h>
#include <drivers/usb.h>
#include <ipc/channel.h>
#include <obj/namespace.h>
#include <obj/object.h>
#include <mm/kheap.h>
#include <proc/wait.h>
#include <lib/io.h>
#include <lib/string.h>
#include <arch/types.h>
#include <arch/interrupts.h>

//USB HID modifier byte bits (keyboard report byte 0)
#define HID_MOD_LEFT_CTRL   (1 << 0)
#define HID_MOD_LEFT_SHIFT  (1 << 1)
#define HID_MOD_LEFT_ALT    (1 << 2)
#define HID_MOD_LEFT_GUI    (1 << 3)
#define HID_MOD_RIGHT_CTRL  (1 << 4)
#define HID_MOD_RIGHT_SHIFT (1 << 5)
#define HID_MOD_RIGHT_ALT   (1 << 6)
#define HID_MOD_RIGHT_GUI   (1 << 7)

//USB HID usage code -> ASCII (unshifted, printable range only)
//index = USB HID usage code; value = ASCII character or 0
static const char hid_keycode_to_ascii[256] = {
    //0x00-0x03: reserved / error
    0, 0, 0, 0,
    //0x04-0x1D: a-z
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    //0x1E-0x27: 1-9, 0
    '1','2','3','4','5','6','7','8','9','0',
    //0x28-0x38: enter, esc, backspace, tab, space, symbols
    '\n', 27, '\b', '\t', ' ',
    '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/',
    //0x39: caps lock
    0,
    //0x3A-0x45: F1-F12
    0,0,0,0,0,0,0,0,0,0,0,0,
    //0x46-0x4F: print screen, scroll lock, pause, insert, home, pgup, del, end, pgdn, right
    0,0,0,0,0,0,0,0,0,0,
    //0x50-0x52: left, down, up (arrows)
    0,0,0,
    //0x53: num lock
    0,
    //0x54-0x63: numpad / * - + enter 1-9 0 .
    '/','*','-','+','\n',
    '1','2','3','4','5','6','7','8','9','0','.',
    //0x64-0xFF: the rest are 0
};

//shifted equivalents for the symbols above
static const char hid_keycode_to_ascii_shift[256] = {
    0, 0, 0, 0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 27, '\b', '\t', ' ',
    '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,
    0,
    '/','*','-','+','\n',
    '1','2','3','4','5','6','7','8','9','0','.',
};

//channel endpoint handles - resolved lazily from the namespace so
//HID works regardless of whether PS/2 keyboard/mouse are present
static channel_endpoint_t *s_kbd_ep  = NULL;
static channel_endpoint_t *s_mouse_ep = NULL;
static volatile bool s_usb_kbd_active = false;
static volatile bool s_usb_mouse_active = false;
static bool s_ps2_kbd_masked = false;
static bool s_ps2_mouse_masked = false;

static bool channel_queue_full(channel_endpoint_t *ep) {
    if (!ep) return true;
    channel_t *ch = ep->channel;
    int peer_id = 1 - ep->endpoint_id;
    irq_state_t flags = spinlock_irq_acquire(&ch->lock);
    bool full = (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE);
    spinlock_irq_release(&ch->lock, flags);
    return full;
}

static channel_endpoint_t *get_kbd_ep(void) {
    if (s_kbd_ep) return s_kbd_ep;
    object_t *obj = ns_lookup("$devices/keyboard/channel");
    if (!obj) return NULL;
    //the namespace holds the CLIENT endpoint (id=0); we need to push from
    //the SERVER side (id=1) so events land in queue[0], which the client reads
    channel_endpoint_t *client_ep = (channel_endpoint_t *)obj->data;
    object_deref(obj);
    s_kbd_ep = &client_ep->channel->endpoints[1 - client_ep->endpoint_id];
    return s_kbd_ep;
}

static channel_endpoint_t *get_mouse_ep(void) {
    if (s_mouse_ep) return s_mouse_ep;
    object_t *obj = ns_lookup("$devices/mouse/channel");
    if (!obj) return NULL;
    //use the server (peer) endpoint so events reach the client's queue
    channel_endpoint_t *client_ep = (channel_endpoint_t *)obj->data;
    object_deref(obj);
    s_mouse_ep = &client_ep->channel->endpoints[1 - client_ep->endpoint_id];
    return s_mouse_ep;
}

//generic channel push (mirrors the pattern in mouse.c / keyboard.c)
static void push_to_channel(channel_endpoint_t *ep, void *data, uint32 len) {
    if (!ep) return;

    channel_t *ch      = ep->channel;
    int        peer_id = 1 - ep->endpoint_id;

    //fast path: if queue is already full, drop without heap churn
    irq_state_t flags = spinlock_irq_acquire(&ch->lock);
    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        spinlock_irq_release(&ch->lock, flags);
        kfree(data);
        return;
    }
    spinlock_irq_release(&ch->lock, flags);

    channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
    if (!entry) { kfree(data); return; }

    entry->data     = data;
    entry->data_len = len;
    entry->next     = NULL;

    flags = spinlock_irq_acquire(&ch->lock);

    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        spinlock_irq_release(&ch->lock, flags);
        kfree(entry);
        kfree(data);
        return;
    }

    if (ch->queue_tail[peer_id])
        ch->queue_tail[peer_id]->next = entry;
    else
        ch->queue[peer_id] = entry;
    ch->queue_tail[peer_id] = entry;
    ch->queue_len[peer_id]++;

    thread_wake_one(&ch->waiters[peer_id]);
    spinlock_irq_release(&ch->lock, flags);
}

//keyboard report processing
//track the previous report so we can synthesise key-up events
static uint8 s_prev_kbd_report[8];

static uint8 hid_mods_to_kbd_mods(uint8 hid_mod) {
    uint8 mods = 0;
    if (hid_mod & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) mods |= KBD_MOD_SHIFT;
    if (hid_mod & (HID_MOD_LEFT_CTRL  | HID_MOD_RIGHT_CTRL))  mods |= KBD_MOD_CTRL;
    if (hid_mod & (HID_MOD_LEFT_ALT   | HID_MOD_RIGHT_ALT))   mods |= KBD_MOD_ALT;
    return mods;
}

static void hid_push_key(uint8 keycode, uint8 mods, uint8 pressed) {
    channel_endpoint_t *ep = get_kbd_ep();
    if (!ep) return;
    if (channel_queue_full(ep)) return;

    kbd_event_t *ev = kmalloc(sizeof(kbd_event_t));
    if (!ev) return;

    bool shift = !!(mods & KBD_MOD_SHIFT);
    char ascii = pressed
        ? (shift ? hid_keycode_to_ascii_shift[keycode]
                 : hid_keycode_to_ascii[keycode])
        : 0;

    ev->keycode   = keycode;
    ev->mods      = mods;
    ev->pressed   = pressed;
    ev->_pad      = 0;
    ev->codepoint = (uint32)(unsigned char)ascii;

    push_to_channel(ep, ev, sizeof(kbd_event_t));
}

static void hid_handle_keyboard(const uint8 *report, uint32 len) {
    if (len < 3) return;

    uint8 new_mod = report[0];
    uint8 old_mod = s_prev_kbd_report[0];
    uint8 mods    = hid_mods_to_kbd_mods(new_mod);

    //synthesise key-up events for keys in the old report but not the new one
    for (int i = 2; i < 8; i++) {
        uint8 old_key = s_prev_kbd_report[i];
        if (old_key == 0) continue;
        bool still_held = false;
        for (int j = 2; j < 8; j++) {
            if ((j < (int)len) && report[j] == old_key) { still_held = true; break; }
        }
        if (!still_held)
            hid_push_key(old_key, hid_mods_to_kbd_mods(old_mod), 0);
    }

    //synthesise key-down events for keys in the new report but not the old one
    for (int i = 2; i < (int)len && i < 8; i++) {
        uint8 new_key = report[i];
        if (new_key == 0) continue;
        bool was_held = false;
        for (int j = 2; j < 8; j++) {
            if (s_prev_kbd_report[j] == new_key) { was_held = true; break; }
        }
        if (!was_held)
            hid_push_key(new_key, mods, 1);
    }

    //save report for next comparison
    memset(s_prev_kbd_report, 0, sizeof(s_prev_kbd_report));
    uint32 copy = (len < 8) ? len : 8;
    memcpy(s_prev_kbd_report, report, copy);
}

//mouse report processing
static void hid_handle_mouse(const uint8 *report, uint32 len) {
    if (len < 3) return;

    channel_endpoint_t *ep = get_mouse_ep();
    if (!ep) return;
    if (channel_queue_full(ep)) return;

    mouse_event_t *ev = kmalloc(sizeof(mouse_event_t));
    if (!ev) return;

    uint8  btn_raw = report[0];
    int8   raw_dx  = (int8)report[1];
    int8   raw_dy  = (int8)report[2];

    ev->buttons = btn_raw & 0x07;       //bits 0-2: L/R/M
    ev->dx      = (int16)raw_dx;
    ev->dy      = (int16)raw_dy;        //HID Y: positive = down; screen coords: positive = down
    ev->_pad[0] = ev->_pad[1] = ev->_pad[2] = 0;

    push_to_channel(ep, ev, sizeof(mouse_event_t));
}

//public entry point called by xhci_process_events()
void hid_report_received(uint8 proto, const void *report, uint32 len) {
    if (!report || len == 0) return;

    const uint8 *r = (const uint8 *)report;

    switch (proto) {
    case HID_PROTO_KEYBOARD:
        if (!s_usb_kbd_active) {
            s_usb_kbd_active = true;
            if (!s_ps2_kbd_masked) {
                interrupt_mask(1);
                s_ps2_kbd_masked = true;
            }
        }
        hid_handle_keyboard(r, len);
        break;
    case HID_PROTO_MOUSE:
        if (!s_usb_mouse_active) {
            s_usb_mouse_active = true;
            if (!s_ps2_mouse_masked) {
                interrupt_mask(12);
                s_ps2_mouse_masked = true;
            }
        }
        hid_handle_mouse(r, len);
        break;
    default:
        break;
    }
}

bool hid_usb_keyboard_active(void) {
    return s_usb_kbd_active;
}

bool hid_usb_mouse_active(void) {
    return s_usb_mouse_active;
}
