#include <arch/types.h>
#include <arch/io.h>
#include <arch/interrupts.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <obj/rights.h>
#include <ipc/channel.h>
#include <proc/process.h>
#include <proc/wait.h>
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

//channel endpoint for pushing events
static channel_endpoint_t *kbd_channel_ep = NULL;

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

//push event to channel
static void kbd_push_event(uint8 keycode, uint8 pressed, uint32 codepoint) {
    if (!kbd_channel_ep) return;

    //send to channel (non-blocking - if queue ful event is lost)
    channel_t *ch = kbd_channel_ep->channel;
    int peer_id = 1 - kbd_channel_ep->endpoint_id;

    //fast drop when queue is already full (avoid heap churn in IRQ context)
    irq_state_t flags = spinlock_irq_acquire(&ch->lock);
    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        spinlock_irq_release(&ch->lock, flags);
        return;
    }
    spinlock_irq_release(&ch->lock, flags);

    //allocate event on heap (channel takes ownership)
    kbd_event_t *event = kmalloc(sizeof(kbd_event_t));
    if (!event) return;

    event->keycode = keycode;
    event->mods = mods;
    event->pressed = pressed;
    event->_pad = 0;
    event->codepoint = codepoint;

    channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
    if (!entry) {
        kfree(event);
        return;
    }

    entry->data = event;
    entry->data_len = sizeof(kbd_event_t);
    entry->next = NULL;

    //lock again to enqueue; re-check full due races
    flags = spinlock_irq_acquire(&ch->lock);
    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        spinlock_irq_release(&ch->lock, flags);
        kfree(entry);
        kfree(event);
        return;
    }
    
    //enqueue
    if (ch->queue_tail[peer_id]) {
        ch->queue_tail[peer_id]->next = entry;
    } else {
        ch->queue[peer_id] = entry;
    }
    ch->queue_tail[peer_id] = entry;
    ch->queue_len[peer_id]++;

    //wake any thread waiting for a message
    thread_wake_one(&ch->waiters[peer_id]);
    spinlock_irq_release(&ch->lock, flags);
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
    
    //create channel for keyboard events
    process_t *kproc = process_get_kernel();
    if (kproc) {
        int32 client_ep, server_ep;
        if (channel_create(kproc, HANDLE_RIGHTS_DEFAULT, &client_ep, &server_ep) == 0) {
            //server endpoint is where we push events FROM
            kbd_channel_ep = channel_get_endpoint(kproc, server_ep);
            
            //client endpoint is what userspace opens to receive events
            object_t *client_obj = process_get_handle(kproc, client_ep);
            if (client_obj) {
                ns_register("$devices/keyboard/channel", client_obj);
            }
        }
    }
}

DECLARE_DRIVER(keyboard_init, INIT_LEVEL_DEVICE);
