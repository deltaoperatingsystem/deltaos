#ifndef IPC_CHANNEL_H
#define IPC_CHANNEL_H

#include <arch/types.h>
#include <obj/object.h>
#include <obj/rights.h>
#include <proc/wait.h>

/*
 *channels - fuchsia-style (zircon) IPC primitive
 * 
 *a channel is just a bidirectional IPC mechanism with two endpoints
 *messages can carry data and handles (capabilities)
 *when handles are transferred they are MOVED (not copied) from
 *sender to receive this enforces capability discipline
 */

#define CHANNEL_MAX_MSG_SIZE    4096
#define CHANNEL_MAX_MSG_HANDLES 64
#define CHANNEL_MSG_QUEUE_SIZE  16

//forward declarations
struct process;
struct channel;

//message structure (for sending/receiving)
typedef struct channel_msg {
    void *data;              //message data (copied)
    size data_len;           //length of data
    int32 *handles;          //array of handles to transfer (for userspace)
    uint32 handle_count;     //number of handles
    uint32 sender_pid;       //PID of the process that sent this message (0 if kernel)
    
    //kernel-side: raw objects for kernel handlers (not for userspace)
    struct object **objects; //transferred objects (with +1 ref)
    handle_rights_t *rights; //rights for each object
    uint32 object_count;
} channel_msg_t;

//internal message queue entry
typedef struct channel_msg_entry {
    void *data; //allocated copy of message data
    size data_len;
    object_t **objects; //transferred objects (already removed from sender)
    handle_rights_t *rights; //rights for each transferred object
    uint32 object_count;
    uint32 sender_pid; //PID of the sending process
    struct channel_msg_entry *next;
} channel_msg_entry_t;

//channel endpoint (one of the two ends)
typedef struct channel_endpoint {
    object_t obj; //kernel object (embedded)
    struct channel *channel; //the channel this belongs to
    int endpoint_id; //0 or 1
    
    //kernel-side handler (for driver/service endpoints)
    void (*handler)(struct channel_endpoint *ep, struct channel_msg *msg, void *ctx);
    void *handler_ctx;
} channel_endpoint_t;

//channel (connects two endpoints)
typedef struct channel {
    channel_endpoint_t endpoints[2];
    
    //message queues (messages sent TO each endpoint)
    channel_msg_entry_t *queue[2]; //head of each queue
    channel_msg_entry_t *queue_tail[2]; //tail for appending
    uint32 queue_len[2]; //current queue length
    
    //wait queues (threads waiting for messages on each endpoint)
    wait_queue_t waiters[2];
    
    //state
    int closed[2]; //1 if endpoint is closed
} channel_t;

//create a channel
//   returns two endpoint handles
//rights specify what rights the handles will have
int channel_create(struct process *proc, 
                   handle_rights_t rights,
                   int32 *out_endpoint0, 
                   int32 *out_endpoint1);

//send a message through a channel endpoint
//handles listed in msg are MOVED from sender (removed from their table)
int channel_send(struct process *proc, int32 endpoint_handle, channel_msg_t *msg);

//receive a message from a channel endpoint
//handles in the message are added to receiver's handle table
//caller must free msg->data after use
int channel_recv(struct process *proc, int32 endpoint_handle, channel_msg_t *msg);

//non-blocking version of channel_recv
int channel_try_recv(struct process *proc, int32 endpoint_handle, channel_msg_t *msg);

//close a channel endpoint
//the peer endpoint will receive a "peer closed" signal
int channel_close(struct process *proc, int32 endpoint_handle);

//check if peer endpoint is closed
int channel_peer_closed(struct process *proc, int32 endpoint_handle);

//get the channel endpoint object from a handle (returns NULL if not a channel)
channel_endpoint_t *channel_get_endpoint(struct process *proc, int32 handle);

#endif
