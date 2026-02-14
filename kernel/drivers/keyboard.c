#include <arch/types.h>
#include <arch/io.h>
#include <arch/interrupts.h>
#include <arch/cpu.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <obj/rights.h>
#include <ipc/channel.h>
#include <proc/process.h>
#include <proc/wait.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <drivers/keyboard_protocol.h>
#include <drivers/ps2.h>
#include <drivers/init.h>

#define KBD_STATUS      0x64
#define KBD_SC          0x60

//scancode flags
#define SC_RELEASE      0x80
#define SC_SHIFT_L      0x2A
#define SC_SHIFT_R      0x36
#define SC_CTRL         0x1D
#define SC_ALT          0x38

//modifier state
static uint8 mods = 0;

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

//push event to channel
static void kbd_push_event(uint8 keycode, uint8 pressed, uint32 codepoint) {
    if (!kbd_channel_ep) return;
    
    //allocate event on heap (channel takes ownership)
    kbd_event_t *event = kmalloc(sizeof(kbd_event_t));
    if (!event) return;
    
    event->keycode = keycode;
    event->mods = mods;
    event->pressed = pressed;
    event->_pad = 0;
    event->codepoint = codepoint;
    
    //send to channel (non-blocking - if queue ful event is lost)
    channel_t *ch = kbd_channel_ep->channel;
    int peer_id = 1 - kbd_channel_ep->endpoint_id;
    
    //allocate queue entry outside the lock
    channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
    if (!entry) {
        kfree(event);
        return;
    }
    
    entry->data = event;
    entry->data_len = sizeof(kbd_event_t);
    entry->next = NULL;
    
    //lock the channel for queue manipulation
    spinlock_irq_acquire(&ch->lock);
    
    //check if queue has space
    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        spinlock_irq_release(&ch->lock);
        kfree(entry);
        kfree(event);  //queue full so drop event
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
    
    spinlock_irq_release(&ch->lock);
    
    //wake any thread waiting for a message
    thread_wake_one(&ch->waiters[peer_id]);
}

void keyboard_irq(void) {
    spinlock_irq_acquire(&ps2_lock);
    uint8 status = inb(KBD_STATUS);
    if (!(status & 1)) {
        spinlock_irq_release(&ps2_lock);
        return;
    }

    uint8 sc = inb(KBD_SC);
    spinlock_irq_release(&ps2_lock);
    bool released = (sc & SC_RELEASE) != 0;
    uint8 code = sc & 0x7F;
    
    //update modifiers
    if (code == SC_SHIFT_L || code == SC_SHIFT_R) {
        if (released) mods &= ~KBD_MOD_SHIFT;
        else mods |= KBD_MOD_SHIFT;
            }
    if (code == SC_CTRL) {
        if (released) mods &= ~KBD_MOD_CTRL;
        else mods |= KBD_MOD_CTRL;
    }
    if (code == SC_ALT) {
        if (released) mods &= ~KBD_MOD_ALT;
        else mods |= KBD_MOD_ALT;
    }

    //get ASCII
    char ascii = (mods & KBD_MOD_SHIFT) ? scancodes_shift[code] : scancodes_normal[code];
    
    //push to channel (for userspace/consumers)
    kbd_push_event(code, !released, (uint32)(unsigned char)ascii);
}

void keyboard_init(void) {
    spinlock_irq_acquire(&ps2_lock);
    //flush any pending scancodes
    while (inb(KBD_STATUS) & 1) {
        inb(KBD_SC);
    }
    spinlock_irq_release(&ps2_lock);
    
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