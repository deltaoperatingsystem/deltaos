#ifndef IPC_CHANNEL_SERVER_H
#define IPC_CHANNEL_SERVER_H

#include <ipc/channel.h>

/*
 *channel server - Kernel-side handler registration for channels
 * 
 *in a monolithic kernel, drivers can register handlers for channel endpoints.
 *when a message is sent to the endpoint, the handler is called immediately
 *(synchronously) before the send syscall returns
 * 
 *andddd this provides RPC-like semantics: client sends request, handler processes
 *it and sends response, client's send returns, client can immediately recv
 */

//handler function type
//  ep:   the endpoint that received the message
//  msg:  the received message (handler takes ownership of msg->data)
//  ctx:  user context passed during registration
typedef void (*channel_handler_t)(channel_endpoint_t *ep, channel_msg_t *msg, void *ctx);

//register a handler for a channel endpoint
//when messages arrive at this endpoint the handler is called immediately
void channel_set_handler(channel_endpoint_t *ep, channel_handler_t handler, void *ctx);

//clear handler for endpoint
void channel_clear_handler(channel_endpoint_t *ep);

//send a response back through the same channel
//this is a convenience for handlers - sends to the peer endpoint
int channel_reply(channel_endpoint_t *ep, channel_msg_t *msg);

#endif
