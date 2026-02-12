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

//send command to mouse (via PS/2 controller port 2)
static void mouse_write(uint8 cmd) {
    spinlock_irq_acquire(&ps2_lock);
    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_WRITE_PORT2);
    ps2_wait_write();
    outb(PS2_DATA, cmd);
    spinlock_irq_release(&ps2_lock);
}

//read response from mouse
static uint8 mouse_read(void) {
    spinlock_irq_acquire(&ps2_lock);
    ps2_wait_read();
    uint8 data = inb(PS2_DATA);
    spinlock_irq_release(&ps2_lock);
    return data;
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
    
    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        kfree(event);
        return;
    }
    
    channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
    if (!entry) {
        kfree(event);
        return;
    }
    
    entry->data = event;
    entry->data_len = sizeof(mouse_event_t);
    entry->next = NULL;
    
    if (ch->queue_tail[peer_id]) {
        ch->queue_tail[peer_id]->next = entry;
    } else {
        ch->queue[peer_id] = entry;
    }
    ch->queue_tail[peer_id] = entry;
    ch->queue_len[peer_id]++;
    
    thread_wake_one(&ch->waiters[peer_id]);
}

void mouse_irq(void) {
    spinlock_irq_acquire(&ps2_lock);
    uint8 status = inb(PS2_STATUS);
    
    //bit 5 must be set for mouse data bit 0 for data available
    if (!(status & 0x21)) {
        spinlock_irq_release(&ps2_lock);
        return;
    }
    
    uint8 data = inb(PS2_DATA);
    spinlock_irq_release(&ps2_lock);
    
    switch (mouse_cycle) {
        case 0:
            //first byte: buttons and sign bits
            //bit 3 must always be set (sync bit)
            if (!(data & 0x08)) {
                //out of sync - skip until we find a valid first byte
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
            //third byte: y movement - packet complete
            mouse_packet[2] = data;
            mouse_cycle = 0;
            
            //decode packet
            uint8 buttons = mouse_packet[0] & 0x07;  //lower 3 bits
            
            int16 dx = mouse_packet[1];
            int16 dy = mouse_packet[2];
            
            //apply sign extension from first byte
            if (mouse_packet[0] & 0x10) dx |= 0xFF00;  //x sign bit
            if (mouse_packet[0] & 0x20) dy |= 0xFF00;  //y sign bit
            
            //PS/2 mouse Y is inverted (positive = up)
            dy = -dy;
            
            //check for overflow (discard packet)
            if ((mouse_packet[0] & 0xC0) == 0) {
                mouse_push_event(dx, dy, buttons);
            }
            break;
    }
}

void mouse_init(void) {
    //unmask cascade IRQ - required for any PIC2 interrupt to work
    pic_clear_mask(2);
    
    //enable mouse port on PS/2 controller
    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_ENABLE_PORT2);
    
    //read controller config
    spinlock_irq_acquire(&ps2_lock);
    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_READ_CONFIG);
    ps2_wait_read();
    uint8 config = inb(PS2_DATA);
    
    //enable IRQ12 (bit 1) and keep keybnoard enabled (bit 0)
    config |= 0x02;   //enable IRQ12
    config &= ~0x20;  //enable mouse clock
    
    ps2_wait_write();
    outb(PS2_CMD, PS2_CMD_WRITE_CONFIG);
    ps2_wait_write();
    outb(PS2_DATA, config);
    spinlock_irq_release(&ps2_lock);
    
    //reset mouse to defaults
    mouse_write(MOUSE_CMD_SET_DEFAULTS);
    mouse_read();  //ACK
    
    //enable data reporting
    mouse_write(MOUSE_CMD_ENABLE);
    mouse_read();  //ACK
    
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
