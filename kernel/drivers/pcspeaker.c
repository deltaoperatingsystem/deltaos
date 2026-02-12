#include <drivers/pcspeaker.h>
#include <arch/io.h>
#include <lib/io.h>
#include <ipc/channel.h>
#include <proc/process.h>
#include <obj/namespace.h>
#include <obj/rights.h>
#include <lib/string.h>
#include <drivers/init.h>

#define PIT_CMD    0x43
#define PIT_CHAN2  0x42
#define PORT_B     0x61

static void speaker_handler(channel_endpoint_t *ep, channel_msg_t *msg, void *ctx) {
    (void)ep; (void)ctx;
    if (msg->data_len >= sizeof(uint32)) {
        uint32 freq = *(uint32 *)msg->data;
        pcspeaker_beep(freq);
    }
}

void pcspeaker_init(void) {
    //make sure the speaker is off at start
    pcspeaker_stop();

    //create channel for speaker commands
    process_t *kproc = process_get_kernel();
    if (kproc) {
        int32 client_ep, server_ep;
        //we need READ/WRITE rights for the channel
        if (channel_create(kproc, HANDLE_RIGHTS_DEFAULT, &client_ep, &server_ep) == 0) {
            channel_endpoint_t *sep = channel_get_endpoint(kproc, server_ep);
            if (sep) {
                channel_set_handler(sep, speaker_handler, NULL);
            }
            
            object_t *client_obj = process_get_handle(kproc, client_ep);
            if (client_obj) {
                ns_register("$devices/speaker/channel", client_obj);
            }
        }
    }
}

DECLARE_DRIVER(pcspeaker_init, INIT_LEVEL_DEVICE);

void pcspeaker_beep(uint32 freq) {
    if (freq == 0) {
        pcspeaker_stop();
        return;
    }

    uint32 div = 1193180 / freq;
    
    //set PIT to mode 3 (square wave) on channel 2
    outb(PIT_CMD, 0xB6);
    
    //set divisor
    outb(PIT_CHAN2, (uint8)(div & 0xFF));
    outb(PIT_CHAN2, (uint8)((div >> 8) & 0xFF));
    
    //enable speaker by setting bits 0 and 1 of port 0x61
    uint8 tmp = inb(PORT_B);
    if (tmp != (tmp | 3)) {
        outb(PORT_B, tmp | 3);
    }
}

void pcspeaker_stop(void) {
    uint8 tmp = inb(PORT_B) & 0xFC; //clear bits 0 and 1
    outb(PORT_B, tmp);
}
