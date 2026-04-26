#include <drivers/hid.h>
#include <drivers/keyboard.h>
#include <drivers/keyboard_protocol.h>
#include <drivers/mouse_protocol.h>
#include <drivers/usb/usb.h>
#include <ipc/channel.h>
#include <obj/namespace.h>
#include <obj/object.h>
#include <mm/kheap.h>
#include <proc/event.h>
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

//parsed report descriptor profile for the active keyboard/mouse report stream
typedef struct {
    bool   valid;
    bool   has_report_id;
    uint8  report_id;
    uint16 report_len;
    bool   kbd_layout_valid;
    uint16 kbd_mods_off;
    uint8  kbd_mods_bits;
    uint16 kbd_keys_off;
    uint8  kbd_keys_count;
    bool   mouse_layout_valid;
    uint16 mouse_buttons_off;
    uint8  mouse_buttons_bits;
    uint16 mouse_x_off;
    uint8  mouse_x_bits;
    uint16 mouse_y_off;
    uint8  mouse_y_bits;
    uint16 mouse_wheel_off;
    uint8  mouse_wheel_bits;
} hid_report_profile_t;

static hid_report_profile_t s_report_profile[3];

#define HID_ITEM_TYPE_MAIN   0
#define HID_ITEM_TYPE_GLOBAL 1
#define HID_ITEM_TYPE_LOCAL  2

#define HID_MAIN_INPUT           8
#define HID_MAIN_COLLECTION     10
#define HID_MAIN_END_COLLECTION  12

#define HID_GLOBAL_USAGE_PAGE     0
#define HID_GLOBAL_REPORT_SIZE    7
#define HID_GLOBAL_REPORT_ID      8
#define HID_GLOBAL_REPORT_COUNT    9

#define HID_LOCAL_USAGE           0

#define HID_COLLECTION_APPLICATION 1

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_KEYBOARD       0x07
#define HID_USAGE_PAGE_BUTTON         0x09

#define HID_USAGE_MOUSE               0x02
#define HID_USAGE_KEYBOARD            0x06

#define HID_USAGE_X                   0x30
#define HID_USAGE_Y                   0x31
#define HID_USAGE_WHEEL               0x38

static uint32 hid_read_bits(const uint8 *report, uint32 bit_off, uint8 bits) {
    uint32 value = 0;
    for (uint8 i = 0; i < bits; i++) {
        uint32 bit = bit_off + i;
        if (report[bit >> 3] & (1U << (bit & 7))) {
            value |= (1U << i);
        }
    }
    return value;
}

static int32 hid_read_signed_bits(const uint8 *report, uint32 bit_off, uint8 bits) {
    if (bits == 0) return 0;
    uint32 value = hid_read_bits(report, bit_off, bits);
    if (bits < 32 && (value & (1U << (bits - 1)))) {
        value |= ~((1U << bits) - 1U);
    }
    return (int32)value;
}

static uint32 hid_item_value(const uint8 *p, uint8 size) {
    switch (size) {
    case 0: return 0;
    case 1: return p[1];
    case 2: return (uint32)p[1] | ((uint32)p[2] << 8);
    case 3: return (uint32)p[1] | ((uint32)p[2] << 8) | ((uint32)p[3] << 16) | ((uint32)p[4] << 24);
    default: return 0;
    }
}

static bool hid_parse_and_store_profile(const void *desc, uint32 len, uint8 *proto_out) {
    if (!desc || len == 0) return false;
    if (proto_out) *proto_out = 0;

    const uint8 *p = (const uint8 *)desc;
    const uint8 *end = p + len;

    uint8  usage_page = 0;
    uint32 usage = 0;
    bool   have_usage = false;
    uint8  report_size = 0;
    uint8  report_count = 0;
    uint8  report_id = 0;
    bool   has_report_id = false;
    bool   report_id_seen = false;
    uint32 report_bits = 0;
    uint8  proto = 0;
    uint32 collection_depth = 0;
    uint32 app_depth = 0;
    uint8  seen_report_id = 0;
    uint32 report_offset_bits = 0;
    uint32 local_usages[16];
    uint8  local_usage_count = 0;
    uint32 usage_min = 0;
    uint32 usage_max = 0;
    bool   have_usage_min = false;
    bool   have_usage_max = false;
    bool   have_local_usage = false;
    uint32 local_usage = 0;

    hid_report_profile_t profile = {0};

    while (p < end) {
        uint8 prefix = *p++;
        if (prefix == 0xFE) {
            if (p + 1 >= end) break;
            uint8 data_len = p[0];
            p += 2;
            if (p + data_len > end) break;
            p += data_len;
            continue;
        }

        uint8 size_code = prefix & 0x03;
        uint8 size = (size_code == 3) ? 4 : size_code;
        uint8 type = (prefix >> 2) & 0x03;
        uint8 tag  = (prefix >> 4) & 0x0F;

        if (p + size > end) break;
        uint32 value = hid_item_value(p - 1, size);
        p += size;

        switch (type) {
        case HID_ITEM_TYPE_GLOBAL:
            switch (tag) {
            case HID_GLOBAL_USAGE_PAGE:
                usage_page = (uint8)value;
                break;
            case HID_GLOBAL_REPORT_SIZE:
                report_size = (uint8)value;
                break;
            case HID_GLOBAL_REPORT_ID:
                report_id = (uint8)value;
                has_report_id = true;
                if (!report_id_seen) {
                    seen_report_id = report_id;
                    report_id_seen = true;
                } else if (report_id != seen_report_id) {
                    //multiple distinct report IDs are outside what this stack handles
                    return false;
                }
                break;
            case HID_GLOBAL_REPORT_COUNT:
                report_count = (uint8)value;
                break;
            default:
                break;
            }
            break;

        case HID_ITEM_TYPE_LOCAL:
            switch (tag) {
            case HID_LOCAL_USAGE:
                usage = value;
                have_usage = true;
                if (local_usage_count < (uint8)(sizeof(local_usages) / sizeof(local_usages[0]))) {
                    local_usages[local_usage_count++] = value;
                }
                have_local_usage = true;
                local_usage = value;
                have_usage_min = false;
                have_usage_max = false;
                break;
            case 1: //USAGE_MINIMUM
                usage_min = value;
                have_usage_min = true;
                break;
            case 2: //USAGE_MAXIMUM
                usage_max = value;
                have_usage_max = true;
                break;
            default:
                break;
            }
            break;

        case HID_ITEM_TYPE_MAIN:
            switch (tag) {
            case HID_MAIN_COLLECTION:
                if (collection_depth == 0 && value == HID_COLLECTION_APPLICATION &&
                    have_usage && usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                    if (usage == HID_USAGE_MOUSE) {
                        proto = HID_PROTO_MOUSE;
                        app_depth = collection_depth + 1;
                    } else if (usage == HID_USAGE_KEYBOARD) {
                        proto = HID_PROTO_KEYBOARD;
                        app_depth = collection_depth + 1;
                    }
                }
                collection_depth++;
                have_usage = false;
                local_usage_count = 0;
                break;

            case HID_MAIN_END_COLLECTION:
                if (collection_depth > 0) collection_depth--;
                break;

            case HID_MAIN_INPUT:
                if (proto != 0 && collection_depth >= app_depth) {
                    report_bits += (uint32)report_size * (uint32)report_count;
                    if (proto == HID_PROTO_KEYBOARD) {
                        if (usage_page == HID_USAGE_PAGE_KEYBOARD && report_size == 1 && report_count >= 8) {
                            profile.kbd_layout_valid = true;
                            profile.kbd_mods_off = (uint16)report_offset_bits;
                            profile.kbd_mods_bits = 8;
                        } else if (usage_page == HID_USAGE_PAGE_KEYBOARD && report_size == 8 && report_count > 0) {
                            profile.kbd_layout_valid = true;
                            profile.kbd_keys_off = (uint16)report_offset_bits;
                            profile.kbd_keys_count = report_count;
                            if (profile.kbd_keys_count > 16) profile.kbd_keys_count = 16;
                        }
                    } else if (proto == HID_PROTO_MOUSE) {
                        if (usage_page == HID_USAGE_PAGE_BUTTON && report_count > 0) {
                            profile.mouse_layout_valid = true;
                            profile.mouse_buttons_off = (uint16)report_offset_bits;
                            profile.mouse_buttons_bits = report_count;
                            if (profile.mouse_buttons_bits > 8) profile.mouse_buttons_bits = 8;
                        } else if (usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                            for (uint8 i = 0; i < report_count; i++) {
                                uint32 field_usage = 0;
                                if (i < local_usage_count) {
                                    field_usage = local_usages[i];
                                } else if (have_usage_min && have_usage_max && (usage_min + i) <= usage_max) {
                                    field_usage = usage_min + i;
                                } else if (have_local_usage) {
                                    field_usage = local_usage;
                                }

                                uint16 field_off = (uint16)(report_offset_bits + (uint32)i * report_size);
                                switch (field_usage) {
                                case HID_USAGE_X:
                                    profile.mouse_layout_valid = true;
                                    profile.mouse_x_off = field_off;
                                    profile.mouse_x_bits = report_size;
                                    break;
                                case HID_USAGE_Y:
                                    profile.mouse_layout_valid = true;
                                    profile.mouse_y_off = field_off;
                                    profile.mouse_y_bits = report_size;
                                    break;
                                case HID_USAGE_WHEEL:
                                    profile.mouse_layout_valid = true;
                                    profile.mouse_wheel_off = field_off;
                                    profile.mouse_wheel_bits = report_size;
                                    break;
                                default:
                                    break;
                                }
                            }
                        }
                    }
                }
                report_offset_bits += (uint32)report_size * (uint32)report_count;
                local_usage_count = 0;
                have_local_usage = false;
                have_usage_min = false;
                have_usage_max = false;
                break;

            default:
                break;
            }
            break;
        }
    }

    if (proto == 0) return false;

    uint32 report_bytes = (report_bits + 7U) / 8U;
    if (has_report_id) report_bytes += 1;
    if (report_bytes == 0) return false;

    if (report_bytes > 0xFFFF) report_bytes = 0xFFFF;

    profile.valid = true;
    profile.has_report_id = has_report_id;
    profile.report_id = report_id;
    profile.report_len = (uint16)report_bytes;
    s_report_profile[proto] = profile;
    if (proto_out) *proto_out = proto;
    return true;
}

bool hid_parse_report_descriptor(const void *desc, uint32 len,
                                 uint8 *proto_out,
                                 uint8 *report_id_out,
                                 uint16 *report_len_out) {
    if (proto_out) *proto_out = 0;
    if (report_id_out) *report_id_out = 0;
    if (report_len_out) *report_len_out = 0;

    uint8 proto = 0;
    if (!hid_parse_and_store_profile(desc, len, &proto)) return false;

    if (proto_out) *proto_out = proto;
    if (report_id_out) *report_id_out = s_report_profile[proto].has_report_id
        ? s_report_profile[proto].report_id : 0;
    if (report_len_out) *report_len_out = s_report_profile[proto].report_len;
    return true;
}

static bool hid_strip_report_id(uint8 proto, const uint8 **report, uint32 *len) {
    if (!report || !*report || !len) return false;
    if (proto < HID_PROTO_KEYBOARD || proto > HID_PROTO_MOUSE) return false;

    hid_report_profile_t *profile = &s_report_profile[proto];
    if (!profile->valid) return true;

    if (profile->has_report_id) {
        if (*len == 0) return false;
        if ((*report)[0] != profile->report_id) return false;
        *report += 1;
        *len -= 1;
    }

    if (profile->report_len > 0 && *len < (uint32)profile->report_len - (profile->has_report_id ? 1U : 0U)) {
        return false;
    }
    return true;
}

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
static uint8 s_prev_kbd_report[64];
static uint8 s_prev_kbd_report_len = 0;

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

    if (pressed && (mods & KBD_MOD_CTRL) && (ascii == 'c' || ascii == 'C')) {
        keyboard_queue_interrupt(proc_get_console_foreground_pid());
    }

    push_to_channel(ep, ev, sizeof(kbd_event_t));
}

static void hid_handle_keyboard(const uint8 *report, uint32 len) {
    if (!hid_strip_report_id(HID_PROTO_KEYBOARD, &report, &len)) return;
    if (len == 0) return;

    hid_report_profile_t *profile = &s_report_profile[HID_PROTO_KEYBOARD];

    uint8 new_mod = 0;
    uint8 old_mod = 0;
    uint8 mods = 0;
    uint8 new_keys[16] = {0};
    uint8 old_keys[16] = {0};
    uint8 key_count = 0;

    if (profile->valid && profile->kbd_layout_valid &&
        profile->kbd_mods_bits > 0 && profile->kbd_keys_count > 0) {
        uint8 mod_bits = (profile->kbd_mods_bits > 8) ? 8 : profile->kbd_mods_bits;
        new_mod = (uint8)hid_read_bits(report, profile->kbd_mods_off, mod_bits);
        if (s_prev_kbd_report_len > 0) {
            old_mod = (uint8)hid_read_bits(s_prev_kbd_report, profile->kbd_mods_off, mod_bits);
        }
        mods = hid_mods_to_kbd_mods(new_mod);

        key_count = profile->kbd_keys_count;
        if (key_count > 16) key_count = 16;
        for (uint8 i = 0; i < key_count; i++) {
            new_keys[i] = (uint8)hid_read_bits(report, profile->kbd_keys_off + (uint32)i * 8, 8);
            if (s_prev_kbd_report_len > 0) {
                old_keys[i] = (uint8)hid_read_bits(s_prev_kbd_report, profile->kbd_keys_off + (uint32)i * 8, 8);
            }
        }
    } else {
        if (len < 8) return;
        new_mod = report[0];
        old_mod = s_prev_kbd_report[0];
        mods    = hid_mods_to_kbd_mods(new_mod);
        key_count = 6;
        for (uint8 i = 0; i < key_count; i++) {
            new_keys[i] = report[2 + i];
            old_keys[i] = s_prev_kbd_report[2 + i];
        }
    }

    //synthesise key-up events for keys in the old report but not the new one
    for (int i = 0; i < (int)key_count; i++) {
        uint8 old_key = old_keys[i];
        if (old_key == 0) continue;
        bool still_held = false;
        for (int j = 0; j < (int)key_count; j++) {
            if (new_keys[j] == old_key) { still_held = true; break; }
        }
        if (!still_held)
            hid_push_key(old_key, hid_mods_to_kbd_mods(old_mod), 0);
    }

    //synthesise key-down events for keys in the new report but not the old one
    for (int i = 0; i < (int)key_count; i++) {
        uint8 new_key = new_keys[i];
        if (new_key == 0) continue;
        bool was_held = false;
        for (int j = 0; j < (int)key_count; j++) {
            if (old_keys[j] == new_key) { was_held = true; break; }
        }
        if (!was_held)
            hid_push_key(new_key, mods, 1);
    }

    //save report for next comparison
    memset(s_prev_kbd_report, 0, sizeof(s_prev_kbd_report));
    uint32 copy = (len < sizeof(s_prev_kbd_report)) ? len : sizeof(s_prev_kbd_report);
    memcpy(s_prev_kbd_report, report, copy);
    s_prev_kbd_report_len = (uint8)copy;
}

//mouse report processing
static void hid_handle_mouse(const uint8 *report, uint32 len) {
    if (!hid_strip_report_id(HID_PROTO_MOUSE, &report, &len)) return;

    channel_endpoint_t *ep = get_mouse_ep();
    if (!ep) return;
    if (channel_queue_full(ep)) return;

    mouse_event_t *ev = kmalloc(sizeof(mouse_event_t));
    if (!ev) return;

    hid_report_profile_t *profile = &s_report_profile[HID_PROTO_MOUSE];

    uint8  btn_raw = 0;
    int16  raw_dx  = 0;
    int16  raw_dy  = 0;

    if (profile->valid && profile->mouse_layout_valid &&
        profile->mouse_buttons_bits > 0 &&
        profile->mouse_x_bits > 0 && profile->mouse_y_bits > 0) {
        btn_raw = (uint8)hid_read_bits(report, profile->mouse_buttons_off, profile->mouse_buttons_bits);
        raw_dx  = (int16)hid_read_signed_bits(report, profile->mouse_x_off, profile->mouse_x_bits);
        raw_dy  = (int16)hid_read_signed_bits(report, profile->mouse_y_off, profile->mouse_y_bits);
    } else {
        if (len < 3) {
            kfree(ev);
            return;
        }
        btn_raw = report[0];
        raw_dx  = (int16)(int8)report[1];
        raw_dy  = (int16)(int8)report[2];
    }

    ev->buttons = btn_raw & 0x07;       //bits 0-2: L/R/M
    ev->dx      = raw_dx;
    ev->dy      = raw_dy;        //HID Y: positive = down; screen coords: positive = down
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

void hid_usb_keyboard_detached(void) {
    s_usb_kbd_active = false;
    memset(s_prev_kbd_report, 0, sizeof(s_prev_kbd_report));
    s_prev_kbd_report_len = 0;

    //reset profile
    memset(&s_report_profile[HID_PROTO_KEYBOARD], 0, sizeof(hid_report_profile_t));

    if (s_ps2_kbd_masked) {
        interrupt_unmask(1);
        s_ps2_kbd_masked = false;
    }
}

void hid_usb_mouse_detached(void) {
    s_usb_mouse_active = false;

    //reset profile
    memset(&s_report_profile[HID_PROTO_MOUSE], 0, sizeof(hid_report_profile_t));

    if (s_ps2_mouse_masked) {
        interrupt_unmask(12);
        s_ps2_mouse_masked = false;
    }
}
