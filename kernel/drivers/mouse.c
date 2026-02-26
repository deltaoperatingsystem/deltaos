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
#include <drivers/mouse_protocol.h>
#include <lib/io.h>
#include <drivers/init.h>
#include <drivers/ps2.h>

//PS/2 controller ports
#define PS2_DATA        0x60
#define PS2_STATUS      0x64
#define PS2_CMD         0x64

//PS/2 controller commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT2    0xA8
#define PS2_CMD_WRITE_PORT2     0xD4

//mouse commands
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_DISABLE       0xF5
#define MOUSE_CMD_RESET         0xFF

//channel endpoint for pushing events
static channel_endpoint_t *mouse_channel_ep = NULL;

//mouse packet state (3-byte packets)
static uint8 mouse_packet[3];
static int mouse_cycle = 0;

//wait for PS/2 controller input buffer to be ready
//must be called with ps2_lock held
static void ps2_wait_write(void) {
    int timeout = 100000;
    while ((inb(PS2_STATUS) & 2) && --timeout);
}

//wait for PS/2 controller output buffer to have data
//must be called with ps2_lock held
static void ps2_wait_read(void) {
    int timeout = 100000;
    while (!(inb(PS2_STATUS) & 1) && --timeout);
}

//send a command to the mouse and read its ACK atomically under ps2_lock
//holding the lock across both the write and the read prevents keyboard_irq
//from delegating to mouse_irq in the gap between the two calls, which would
//consume the ACK byte and leave mouse_read spinning until timeout.
static uint8 mouse_cmd(uint8 cmd) {
    irq_state_t flags = spinlock_irq_acquire(&ps2_lock);
    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_WRITE_PORT2);
    ps2_wait_write();
    outb(PS2_DATA, cmd);
    ps2_wait_read();
    uint8 ack = inb(PS2_DATA);
    spinlock_irq_release(&ps2_lock, flags);
    return ack;
}

//push event to channel
static void mouse_push_event(int16 dx, int16 dy, uint8 buttons) {
    if (!mouse_channel_ep) {
        printf("[mouse_push] no channel_ep!\n");
        return;
    }
    
    mouse_event_t *event = kmalloc(sizeof(mouse_event_t));
    if (!event) return;
    
    event->dx = dx;
    event->dy = dy;
    event->buttons = buttons;
    event->_pad[0] = event->_pad[1] = event->_pad[2] = 0;
    
    channel_t *ch = mouse_channel_ep->channel;
    int peer_id = 1 - mouse_channel_ep->endpoint_id;
    
    //allocate queue entry outside the lock
    channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
    if (!entry) {
        kfree(event);
        return;
    }
    
    entry->data = event;
    entry->data_len = sizeof(mouse_event_t);
    entry->next = NULL;
    
    //lock the channel for queue manipulation
    irq_state_t flags = spinlock_irq_acquire(&ch->lock);
    
    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        spinlock_irq_release(&ch->lock, flags);
        kfree(entry);
        kfree(event);
        return;
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

void mouse_irq(void) {
    irq_state_t flags = spinlock_irq_acquire(&ps2_lock);
    uint8 status = inb(PS2_STATUS);

    //bit 0 = OBF (output buffer full), bit 5 = AUXB (data is from mouse port)
    //both must be set: bail if there is no data, or if the data is from the
    //keyboard port (bit 5 clear) - that byte belongs to keyboard_irq, not us
    if ((status & 0x21) != 0x21) {
        spinlock_irq_release(&ps2_lock, flags);
        return;
    }

    uint8 data = inb(PS2_DATA);

    bool emit = false;
    int16 dx = 0, dy = 0;
    uint8 buttons = 0;

    switch (mouse_cycle) {
        case 0:
            //first byte: buttons and sign bits - bit 3 is always-1 sync bit
            if (!(data & 0x08)) {
                //out of sync; discard and wait for a valid first byte
                spinlock_irq_release(&ps2_lock, flags);
                return;
            }
            mouse_packet[0] = data;
            mouse_cycle = 1;
            break;

        case 1:
            //second byte: x movement
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;

        case 2:
            //third byte: y movement — packet complete
            mouse_packet[2] = data;
            mouse_cycle = 0;

            //decode packet
            buttons = mouse_packet[0] & 0x07;  //lower 3 bits
            dx = (int16)mouse_packet[1];
            dy = (int16)mouse_packet[2];

            //sign-extend from flags byte
            if (mouse_packet[0] & 0x10) dx |= (int16)0xFF00;  //x sign
            if (mouse_packet[0] & 0x20) dy |= (int16)0xFF00;  //y sign

            //PS/2 Y axis is inverted (positive = up on hardware)
            dy = -dy;

            //discard packet if overflow bits are set
            emit = ((mouse_packet[0] & 0xC0) == 0);
            break;
    }

    spinlock_irq_release(&ps2_lock, flags);

    if (emit)
        mouse_push_event(dx, dy, buttons);
}

void mouse_init(void) {
    //hold ps2_lock for the ENTIRE controller init sequence so keyboard_init
    //running concurrently on another CPU cannot interleave its own commands
    irq_state_t flags = spinlock_irq_acquire(&ps2_lock);

    //enable mouse port on PS/2 controller
    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_ENABLE_PORT2);

    //read controller config
    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_READ_CONFIG);
    ps2_wait_read();
    uint8 config = inb(PS2_DATA);

    //enable second port interrupt (bit 1) and clock (clear bit 5)
    config |= (1 << 1);
    config &= ~(1 << 5);

    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_WRITE_CONFIG);
    ps2_wait_write();
    outb(PS2_DATA, config);
    spinlock_irq_release(&ps2_lock, flags);
    
    //reset mouse to defaults and enable data reporting
    //mouse_cmd() holds ps2_lock across the write AND the read so no interrupt
    //handler can sneak in and consume the ACK byte in between
    mouse_cmd(MOUSE_CMD_SET_DEFAULTS);  //returns ACK (0xFA), ignored
    mouse_cmd(MOUSE_CMD_ENABLE);        //returns ACK (0xFA), ignored
    
    //unmask IRQ12 (mouse)
    interrupt_unmask(12);
    
    //create channel for mouse events
    process_t *kproc = process_get_kernel();
    if (kproc) {
        int32 client_ep, server_ep;
        if (channel_create(kproc, HANDLE_RIGHTS_DEFAULT, &client_ep, &server_ep) == 0) {
            mouse_channel_ep = channel_get_endpoint(kproc, server_ep);
            
            object_t *client_obj = process_get_handle(kproc, client_ep);
            if (client_obj) {
                object_ref(client_obj);
                ns_register("$devices/mouse/channel", client_obj);
            }
        }
    }

    puts("[mouse] initialised\n");
}

DECLARE_DRIVER(mouse_init, INIT_LEVEL_DEVICE);
