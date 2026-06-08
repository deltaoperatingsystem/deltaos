#include <arch/types.h>
#include <arch/io.h>
#include <arch/interrupts.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <obj/rights.h>
#include <ipc/channel.h>
#include <proc/process.h>
#include <proc/event.h>
#include <proc/bottom_half.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <drivers/keyboard_protocol.h>
#include <drivers/mouse.h>
#include <drivers/ps2.h>
#include <drivers/hid.h>
#include <drivers/init.h>
#include <lib/io.h>

#define KBD_STATUS      0x64
#define KBD_SC          0x60
#define KBD_CMD         0x64

//8042 controller commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_ENABLE_PORT1    0xAE
#define KBD_DEV_DISABLE_SCAN    0xF5
#define KBD_DEV_ENABLE_SCAN     0xF4
#define KBD_DEV_ACK             0xFA
#define KBD_DEV_RESEND          0xFE
#define KBD_DEV_SELFTEST_OK     0xAA
#define KBD_DEV_ECHO            0xEE
#define KBD_SC_EXTENDED_0       0xE0
#define KBD_SC_EXTENDED_1       0xE1

//scancode flags
#define SC_RELEASE      0x80
#define SC_SHIFT_L      0x2A
#define SC_SHIFT_R      0x36
#define SC_CTRL         0x1D
#define SC_ALT          0x38

//modifier state
static uint8 mods = 0;
static bool extended_prefix = false;
static spinlock_irq_t ctrlc_lock = SPINLOCK_IRQ_INIT;
static uint64 ctrlc_pending_pid = 0;
static bottom_half_handle_t ctrlc_bh = BOTTOM_HALF_INVALID_HANDLE;
static void keyboard_ctrlc_bottom_half(void *arg);

static const char scancodes_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
};

static const char scancodes_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
};

//maximum number of simultaneous keyboard subscribers
#define KBD_MAX_SUBSCRIBERS 8

//one entry per subscriber: the server-side endpoint we push events into
typedef struct {
    channel_endpoint_t *ep;  //server endpoint for this subscriber (NULL = free slot)
    channel_t *ch;           //the channel this endpoint belongs to (cached for safe ISR delivery)
    int32 server_h;          //kernel-held server handle (kept alive to hold channel open)
} kbd_subscriber_t;

static kbd_subscriber_t kbd_subscribers[KBD_MAX_SUBSCRIBERS];
static spinlock_irq_t   kbd_sub_lock = SPINLOCK_IRQ_INIT;

//factory object registered in the namespace - produces a new private channel per opener
static object_t *kbd_factory_obj = NULL;

static object_t *kbd_factory_lookup(object_t *obj, const char *name);

static object_ops_t kbd_factory_ops = {
    .lookup = kbd_factory_lookup,
};

//must be called with ps2_lock held
static void ps2_wait_write(void) {
    //keep this as a tight poll loop during early boot
    //when i tried a timer-based the sleep path was fine in QEMU but could dealdock on some real hardware
    int timeout = 100000;
    while ((inb(KBD_STATUS) & 2) && --timeout);
}

//must be called with ps2_lock held
static void ps2_wait_read(void) {
    //same reason as ps2_wait_write
    int timeout = 100000;
    while (!(inb(KBD_STATUS) & 1) && --timeout);
}

//must be called with ps2_lock held
static void ps2_kbd_flush_locked(void) {
    while (inb(KBD_STATUS) & 1) {
        inb(KBD_SC);
    }
}

//must be called with ps2_lock held
static uint8 ps2_kbd_cmd_locked(uint8 cmd) {
    ps2_wait_write();
    outb(KBD_SC, cmd);
    ps2_wait_read();
    return inb(KBD_SC);
}

//push an event to all live subscribers and prune closed ones
static void kbd_push_event(uint8 keycode, uint8 pressed, uint32 codepoint) {
    kbd_event_t event;
    event.keycode   = keycode;
    event.mods      = mods;
    event.pressed   = pressed;
    event._pad      = 0;
    event.codepoint = codepoint;

    //snapshot chs[] and peer_ids[] for all active subscriber slots
    //ch stays alive for the duration of delivery because:
    //  - server_h is held in kproc -> ch_refcount >= 1 -> channel can't be freed
    //  - pruning (which closes server_h) acquires+releases ch->lock before calling
    //    process_close_handle, serializing against our delivery loop below
    channel_t *chs[KBD_MAX_SUBSCRIBERS];
    int        peer_ids[KBD_MAX_SUBSCRIBERS];
    int ep_count = 0;
    irq_state_t flags = spinlock_irq_acquire(&kbd_sub_lock);
    for (int i = 0; i < KBD_MAX_SUBSCRIBERS; i++) {
        channel_endpoint_t *ep = kbd_subscribers[i].ep;
        if (!ep) continue;
        chs[ep_count]      = kbd_subscribers[i].ch;
        peer_ids[ep_count] = 1 - ep->endpoint_id;  //safe: ep is live while slot is non-NULL
        ep_count++;
    }
    spinlock_irq_release(&kbd_sub_lock, flags);

    //deliver to each subscriber - alloc outside the lock
    for (int i = 0; i < ep_count; i++) {
        channel_t *ch     = chs[i];   //use cached ch pointer - do NOT use ep->channel here
        int peer_id       = peer_ids[i];

        kbd_event_t *ev = kmalloc(sizeof(kbd_event_t));
        if (!ev) continue;
        *ev = event;

        channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
        if (!entry) { kfree(ev); continue; }
        entry->data     = ev;
        entry->data_len = sizeof(kbd_event_t);

        //enqueue under the channel own lock
        flags = spinlock_irq_acquire(&ch->lock);
        if (ch->closed[peer_id] || ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
            spinlock_irq_release(&ch->lock, flags);
            kfree(entry);
            kfree(ev);
            continue;
        }
        if (ch->queue_tail[peer_id]) {
            ch->queue_tail[peer_id]->next = entry;
        } else {
            ch->queue[peer_id] = entry;
        }
        ch->queue_tail[peer_id] = entry;
        ch->queue_len[peer_id]++;
        thread_wake_one(&ch->waiters[peer_id]);
        spinlock_irq_release(&ch->lock, flags);
    }
}

//called when a process opens $devices/keyboard/channel
//creates a fresh private channel, registers the server side as a subscriber
//and returns the client-side object to the opener
static object_t *kbd_factory_lookup(object_t *obj, const char *name) {
    (void)obj;
    if (!name || name[0] != 'c') return NULL;  //only "channel"
    if (name[1] != 'h' || name[2] != 'a' || name[3] != 'n' ||
        name[4] != 'n' || name[5] != 'e' || name[6] != 'l' || name[7] != '\0') return NULL;

    process_t *kproc = process_get_kernel();
    if (!kproc) return NULL;

    //prune dead slots first (safe in process context)
    //wo-phase removal to prevent UAF in the ISR
    //1: clear ep/ch/server_h under the IRQ-safe lock so no new ISR snapshots them
    //2: acquire+release ch->lock to wait for any ISR already mid-delivery, then
    //close the handle (which may free the channel)
    typedef struct { int32 server_h; channel_t *ch; } dead_entry_t;
    dead_entry_t dead[KBD_MAX_SUBSCRIBERS];
    int dead_count = 0;

    irq_state_t flags = spinlock_irq_acquire(&kbd_sub_lock);
    for (int i = 0; i < KBD_MAX_SUBSCRIBERS; i++) {
        channel_endpoint_t *ep = kbd_subscribers[i].ep;
        if (ep && ep->channel->closed[1 - ep->endpoint_id]) {
            dead[dead_count].server_h = kbd_subscribers[i].server_h;
            dead[dead_count].ch       = kbd_subscribers[i].ch;
            dead_count++;
            kbd_subscribers[i].ep      = NULL;
            kbd_subscribers[i].ch      = NULL;
            kbd_subscribers[i].server_h = -1;
        }
    }

    //find a free subscriber slot and reserve it atomically
    int slot = -1;
    for (int i = 0; i < KBD_MAX_SUBSCRIBERS; i++) {
        if (!kbd_subscribers[i].ep && kbd_subscribers[i].server_h == -1) {
            kbd_subscribers[i].server_h = -2;  //sentinel: slot reserved
            slot = i;
            break;
        }
    }
    spinlock_irq_release(&kbd_sub_lock, flags);

    //for each dead slot, flush any in-progress ISR delivery then close the handle
    //acquiring ch->lock serializes against kbd_push_event delivery loop which holds ch->lock
    //while enqueuing, a fter we release ch->lock the ISR is no longer touching ch, so it is
    //safe to call process_close_handle which may free the channel
    for (int i = 0; i < dead_count; i++) {
        if (dead[i].ch) {
            irq_state_t ch_flags = spinlock_irq_acquire(&dead[i].ch->lock);
            spinlock_irq_release(&dead[i].ch->lock, ch_flags);
        }
        if (dead[i].server_h >= 0) {
            process_close_handle(kproc, dead[i].server_h);
        }
    }

    if (slot < 0) return NULL;  //too many subscribers

    //create a new channel pair inside the kernel process
    int32 client_h, server_h;
    if (channel_create(kproc, HANDLE_RIGHTS_DEFAULT, &client_h, &server_h) != 0) {
        //release reserved slot
        flags = spinlock_irq_acquire(&kbd_sub_lock);
        kbd_subscribers[slot].server_h = -1;
        spinlock_irq_release(&kbd_sub_lock, flags);
        return NULL;
    }

    //get the server-side endpoint pointer before we do anything else
    channel_endpoint_t *server_ep = channel_get_endpoint(kproc, server_h);

    //get the client-side object (we will return this to the opener)
    object_t *client_obj = process_get_handle(kproc, client_h);
    if (!client_obj) {
        process_close_handle(kproc, client_h);
        process_close_handle(kproc, server_h);
        //release reserved slot
        flags = spinlock_irq_acquire(&kbd_sub_lock);
        kbd_subscribers[slot].server_h = -1;
        spinlock_irq_release(&kbd_sub_lock, flags);
        return NULL;
    }
    //bump refcount so it stays alive after we close kproc's handle below
    object_ref(client_obj);

    //register server endpoint as subscriber; keep server_h so channel stays alive and we can close it on teardown
    flags = spinlock_irq_acquire(&kbd_sub_lock);
    kbd_subscribers[slot].ep      = server_ep;
    kbd_subscribers[slot].ch      = server_ep->channel;
    kbd_subscribers[slot].server_h = server_h;
    spinlock_irq_release(&kbd_sub_lock, flags);

    //remove client handle from kproc's table - the opener will get their own handle
    process_close_handle(kproc, client_h);

    return client_obj;
}

void keyboard_queue_interrupt(uint64 pid) {
    irq_state_t flags;

    if (pid == 0) return;

    flags = spinlock_irq_acquire(&ctrlc_lock);
    if (ctrlc_pending_pid == 0) {
        ctrlc_pending_pid = pid;
    }
    spinlock_irq_release(&ctrlc_lock, flags);

    if (ctrlc_bh != BOTTOM_HALF_INVALID_HANDLE) {
        bottom_half_raise(ctrlc_bh);
    }
}

//ctrl+c posts are deferred so IRQ context never takes process/event locks
static void keyboard_ctrlc_bottom_half(void *arg) {
    irq_state_t flags;
    uint64 pid;

    (void)arg;

    flags = spinlock_irq_acquire(&ctrlc_lock);
    pid = ctrlc_pending_pid;
    ctrlc_pending_pid = 0;
    spinlock_irq_release(&ctrlc_lock, flags);

    if (pid == 0) {
        return;
    }

    process_t *fg = process_find_ref(pid);
    if (fg) {
        proc_post_event(fg, PROC_EVENT_INTERRUPT);
        process_unref(fg);
    } else {
        proc_clear_console_foreground_if_owner(pid);
    }
}

void keyboard_irq(void) {
    irq_state_t flags = spinlock_irq_acquire(&ps2_lock);
    uint8 status = inb(KBD_STATUS);
    if (!(status & 1)) {
        spinlock_irq_release(&ps2_lock, flags);
        return;
    }
    //if AUX data triggered IRQ1, delegate processing to mouse handler so
    //mouse bytes are not lost on systems with odd PS/2 IRQ routing
    if (status & 0x20) {
        spinlock_irq_release(&ps2_lock, flags);
        mouse_irq();
        return;
    }

    uint8 sc = inb(KBD_SC);
    spinlock_irq_release(&ps2_lock, flags);

    //USB HID keyboard is active: drain legacy PS/2 data and ignore it.
    if (hid_usb_keyboard_active()) {
        return;
    }

    //ignore device replies and extended prefixes
    //rn we only support the simple set-1 make/break path here
    //reating these as keys turns
    //boot-time controller chatter into garbage characters later
    if (sc == KBD_DEV_ACK || sc == KBD_DEV_RESEND || sc == KBD_DEV_ECHO) {
        return;
    }
    if (sc == KBD_DEV_SELFTEST_OK && !(mods & KBD_MOD_SHIFT)) {
        // they clash so if shift wasnt pressed down, probably wasn't the shift sc
        return;
    }
    if (sc == KBD_SC_EXTENDED_0 || sc == KBD_SC_EXTENDED_1) {
        extended_prefix = true;
        return;
    }
    if (extended_prefix) {
        extended_prefix = false;
        bool released = (sc & SC_RELEASE);
        uint8 code = sc & 0x7F;
        //prefix extended keys with 0xE000 so consumers can distinguish them
        kbd_push_event(code, !released, 0xE000 | code);
        return;
    }

    bool released = (sc & SC_RELEASE);
    uint8 code = sc & 0x7F;

    //update modifiers
    if (code == SC_SHIFT_L || code == SC_SHIFT_R) {
        if (released) mods &= ~KBD_MOD_SHIFT;
        else mods |= KBD_MOD_SHIFT;
    } else if (code == SC_CTRL) {
        if (released) mods &= ~KBD_MOD_CTRL;
        else mods |= KBD_MOD_CTRL;
    } else if (code == SC_ALT) {
        if (released) mods &= ~KBD_MOD_ALT;
        else mods |= KBD_MOD_ALT;
    }

    //get ASCII (only on press)
    char ascii = 0;
    if (!released) {
        ascii = (mods & KBD_MOD_SHIFT) ? scancodes_shift[code] : scancodes_normal[code];
    }

    if (!released && (mods & KBD_MOD_CTRL) && (ascii == 'c' || ascii == 'C')) {
        keyboard_queue_interrupt(proc_get_console_foreground_pid());
    }

    //push to channel (for userspace/consumers)
    kbd_push_event(code, !released, (uint32)(unsigned char)ascii);
}

void keyboard_init(void) {
    irq_state_t flags = spinlock_irq_acquire(&ps2_lock);

    //stop the keyboard from queueing more scan codes while we reconfigure the 8042
    (void)ps2_kbd_cmd_locked(KBD_DEV_DISABLE_SCAN);
    ps2_kbd_flush_locked();
    mods = 0;
    extended_prefix = false;

    //initialize subscriber fields (-1 for server_h since 0 is a valid handle)
    for (int i = 0; i < KBD_MAX_SUBSCRIBERS; i++) {
        kbd_subscribers[i].ep       = NULL;
        kbd_subscribers[i].ch       = NULL;
        kbd_subscribers[i].server_h = -1;
    }

    //explicitly enable the first PS/2 port and IRQ1 instead of relying on
    //firmware leaving the 8042 in a keyboard-friendly state
    ps2_wait_write();
    outb(KBD_CMD, PS2_CMD_ENABLE_PORT1);

    ps2_wait_write();
    outb(KBD_CMD, PS2_CMD_READ_CONFIG);
    ps2_wait_read();
    uint8 config = inb(KBD_SC);

    //bit 0 = IRQ1 enable, bit 4 = port 1 clock disable
    config |= (1 << 0);
    config &= ~(1 << 4);

    ps2_wait_write();
    outb(KBD_CMD, PS2_CMD_WRITE_CONFIG);
    ps2_wait_write();
    outb(KBD_SC, config);

    //flush any pending scancodes
    ps2_kbd_flush_locked();

    //re-enable scanning only after the controller and our local parser
    //state are back in sync
    (void)ps2_kbd_cmd_locked(KBD_DEV_ENABLE_SCAN);
    ps2_kbd_flush_locked();
    spinlock_irq_release(&ps2_lock, flags);

    interrupt_unmask(1);

    //create the factory object and register it so that each opener gets their own private channel
    kbd_factory_obj = object_create(OBJECT_DIR, &kbd_factory_ops, NULL);
    if (kbd_factory_obj) {
        ns_register("$devices/keyboard", kbd_factory_obj, HANDLE_RIGHTS_ALL);
    }
}

void keyboard_start(void) {
    //ctrl+c delivery uses the shared bottom-half queue
    if (ctrlc_bh == BOTTOM_HALF_INVALID_HANDLE) {
        ctrlc_bh = bottom_half_register(keyboard_ctrlc_bottom_half, NULL);
    }
}

DECLARE_DRIVER(keyboard_init, INIT_LEVEL_DEVICE);
