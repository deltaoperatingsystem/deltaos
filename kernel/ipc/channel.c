#include <ipc/channel.h>
#include <proc/process.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/spinlock.h>
#include <drivers/serial.h>

static int channel_endpoint_close(object_t *obj) {
    channel_endpoint_t *ep = (channel_endpoint_t *)obj;
    if (!ep || !ep->channel) return -1;
    
    channel_t *ch = ep->channel;
    int id = ep->endpoint_id;
    
    spinlock_irq_acquire(&ch->lock);
    
    //mark this endpoint as closed
    ch->closed[id] = 1;
    
    //wake any threads waiting on either end - their wait state is now invalid
    thread_wake_all(&ch->waiters[id]);
    thread_wake_all(&ch->waiters[1 - id]);

    //free any pending messages in our queue
    channel_msg_entry_t *msg = ch->queue[id];
    while (msg) {
        channel_msg_entry_t *next = msg->next;
        if (msg->data) kfree(msg->data);
        //deref any pending objects
        for (uint32 i = 0; i < msg->object_count; i++) {
            if (msg->objects[i]) object_deref(msg->objects[i]);
        }
        if (msg->objects) kfree(msg->objects);
        if (msg->rights) kfree(msg->rights);
        kfree(msg);
        msg = next;
    }
    ch->queue[id] = NULL;
    ch->queue_tail[id] = NULL;
    ch->queue_len[id] = 0;
    
    //if both endpoints closed just free the channel
    int both_closed = ch->closed[0] && ch->closed[1];
    spinlock_irq_release(&ch->lock);
    
    if (both_closed) {
        kfree(ch);
    }
    
    return 0;
}

static object_ops_t channel_endpoint_ops = {
    .read = NULL,
    .write = NULL,
    .close = channel_endpoint_close,
    .readdir = NULL,
    .lookup = NULL
};

int channel_create(process_t *proc, handle_rights_t rights, 
                   int32 *out_endpoint0, int32 *out_endpoint1) {
    if (!proc || !out_endpoint0 || !out_endpoint1) return -1;
    
    //allocate channel
    channel_t *ch = kzalloc(sizeof(channel_t));
    if (!ch) return -1;
    
    //initialize endpoints (embedded objects)
    for (int i = 0; i < 2; i++) {
        ch->endpoints[i].obj.type = OBJECT_CHANNEL;
        ch->endpoints[i].obj.refcount = 1;
        ch->endpoints[i].obj.ops = &channel_endpoint_ops;
        ch->endpoints[i].obj.data = &ch->endpoints[i];
        ch->endpoints[i].channel = ch;
        ch->endpoints[i].endpoint_id = i;
        ch->endpoints[i].handler = NULL;
        ch->endpoints[i].handler_ctx = NULL;
        ch->queue[i] = NULL;
        ch->queue_tail[i] = NULL;
        ch->queue_len[i] = 0;
        ch->closed[i] = 0;
        wait_queue_init(&ch->waiters[i]);
    }
    spinlock_irq_init(&ch->lock);
    
    //grant handles to process
    int h0 = process_grant_handle(proc, &ch->endpoints[0].obj, rights);
    if (h0 < 0) {
        kfree(ch);
        return -1;
    }
    
    int h1 = process_grant_handle(proc, &ch->endpoints[1].obj, rights);
    if (h1 < 0) {
        process_close_handle(proc, h0);
        kfree(ch);
        return -1;
    }
    
    *out_endpoint0 = h0;
    *out_endpoint1 = h1;
    
    return 0;
}

channel_endpoint_t *channel_get_endpoint(process_t *proc, int32 handle) {
    if (!proc) return NULL;
    
    object_t *obj = process_get_handle(proc, handle);
    if (!obj || obj->type != OBJECT_CHANNEL) return NULL;
    
    return (channel_endpoint_t *)obj;
}

int channel_send(process_t *proc, int32 endpoint_handle, channel_msg_t *msg) {
    if (!proc || !msg) return -1;
    
    channel_endpoint_t *ep = channel_get_endpoint(proc, endpoint_handle);
    if (!ep) return -1;
    
    channel_t *ch = ep->channel;
    int peer_id = 1 - ep->endpoint_id;
    
    //check message size (immutable limits, no lock needed)
    if (msg->data_len > CHANNEL_MAX_MSG_SIZE) {
        return -4;  //message too large
    }
    if (msg->handle_count > CHANNEL_MAX_MSG_HANDLES) {
        return -5;  //too many handles
    }
    
    //allocate queue entry outside the lock
    channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
    if (!entry) return -1;
    
    //copy data
    if (msg->data_len > 0 && msg->data) {
        entry->data = kmalloc(msg->data_len);
        if (!entry->data) {
            kfree(entry);
            return -1;
        }
        memcpy(entry->data, msg->data, msg->data_len);
        entry->data_len = msg->data_len;
    }
    
    //record sender PID
    entry->sender_pid = proc->pid;
    
    //transfer handles (MOVE semantics)
    if (msg->handle_count > 0 && msg->handles) {
        entry->objects = kzalloc(msg->handle_count * sizeof(object_t *));
        entry->rights = kzalloc(msg->handle_count * sizeof(handle_rights_t));
        if (!entry->objects || !entry->rights) {
            if (entry->data) kfree(entry->data);
            if (entry->objects) kfree(entry->objects);
            if (entry->rights) kfree(entry->rights);
            kfree(entry);
            return -1;
        }
        
        entry->object_count = msg->handle_count;
        
        for (uint32 i = 0; i < msg->handle_count; i++) {
            proc_handle_t *he = process_get_handle_entry(proc, msg->handles[i]);
            if (!he) {
                //rollback: restore already-transferred handles
                for (uint32 j = 0; j < i; j++) {
                    if (entry->objects[j]) object_deref(entry->objects[j]);
                }
                if (entry->data) kfree(entry->data);
                kfree(entry->objects);
                kfree(entry->rights);
                kfree(entry);
                return -6;  //invalid handle
            }
            
            //check TRANSFER right
            if (!rights_has(he->rights, HANDLE_RIGHT_TRANSFER)) {
                //rollback
                for (uint32 j = 0; j < i; j++) {
                    if (entry->objects[j]) object_deref(entry->objects[j]);
                }
                if (entry->data) kfree(entry->data);
                kfree(entry->objects);
                kfree(entry->rights);
                kfree(entry);
                return -7;  //no transfer right
            }
            
            //grab object ref and rights
            entry->objects[i] = he->obj;
            entry->rights[i] = he->rights;
            object_ref(he->obj);
            
            //remove from sender (MOVE)
            process_close_handle(proc, msg->handles[i]);
        }
    }
    
    //lock for queue manipulation
    spinlock_irq_acquire(&ch->lock);
    
    //check if peer is closed
    if (ch->closed[peer_id]) {
        spinlock_irq_release(&ch->lock);
        //cleanup the entry we built
        if (entry->data) kfree(entry->data);
        if (entry->objects) {
            for (uint32 i = 0; i < entry->object_count; i++) {
                if (entry->objects[i]) object_deref(entry->objects[i]);
            }
            kfree(entry->objects);
        }
        if (entry->rights) kfree(entry->rights);
        kfree(entry);
        return -2;  //peer closed
    }
    
    //check queue limit
    if (ch->queue_len[peer_id] >= CHANNEL_MSG_QUEUE_SIZE) {
        spinlock_irq_release(&ch->lock);
        if (entry->data) kfree(entry->data);
        if (entry->objects) {
            for (uint32 i = 0; i < entry->object_count; i++) {
                if (entry->objects[i]) object_deref(entry->objects[i]);
            }
            kfree(entry->objects);
        }
        if (entry->rights) kfree(entry->rights);
        kfree(entry);
        return -3;  //queue full
    }
    
    //enqueue message to peer's queue
    entry->next = NULL;
    if (ch->queue_tail[peer_id]) {
        ch->queue_tail[peer_id]->next = entry;
    } else {
        ch->queue[peer_id] = entry;
    }
    ch->queue_tail[peer_id] = entry;
    ch->queue_len[peer_id]++;
    
    //wake any thread waiting for a message on this endpoint
    thread_wake_one(&ch->waiters[peer_id]);
    
    //if peer has a handler registered, call it immediately (synchronous dispatch)
    channel_endpoint_t *peer_ep = &ch->endpoints[peer_id];
    if (peer_ep->handler) {
        //dequeue the message we just enqueued
        channel_msg_t handler_msg;
        memset(&handler_msg, 0, sizeof(handler_msg));
        channel_msg_entry_t *e = ch->queue[peer_id];
        if (e) {
            ch->queue[peer_id] = e->next;
            if (!ch->queue[peer_id]) ch->queue_tail[peer_id] = NULL;
            ch->queue_len[peer_id]--;
            
            handler_msg.data = e->data;
            handler_msg.data_len = e->data_len;
            handler_msg.sender_pid = e->sender_pid;
            handler_msg.handles = NULL;  //not used for kernel handlers
            handler_msg.handle_count = 0;
            
            //pass objects to kernel handler (handler takes ownership)
            handler_msg.objects = e->objects;
            handler_msg.rights = e->rights;
            handler_msg.object_count = e->object_count;
            
            e->data = NULL;     //handler takes ownership
            e->objects = NULL;  //don't free - handler owns them now
            e->rights = NULL;
            kfree(e);
            
            spinlock_irq_release(&ch->lock);
            
            //call handler outside the lock (handler may do its own locking)
            peer_ep->handler(peer_ep, &handler_msg, peer_ep->handler_ctx);
            return 0;
        }
    }
    
    spinlock_irq_release(&ch->lock);
    return 0;
}

int channel_recv(process_t *proc, int32 endpoint_handle, channel_msg_t *msg) {
    if (!proc || !msg) return -1;
    
    channel_endpoint_t *ep = channel_get_endpoint(proc, endpoint_handle);
    if (!ep) return -1;
    
    channel_t *ch = ep->channel;
    int my_id = ep->endpoint_id;
    
    //wait for a message (blocking)
    spinlock_irq_acquire(&ch->lock);
    while (!ch->queue[my_id]) {
        //check if peer closed
        if (ch->closed[1 - my_id]) {
            spinlock_irq_release(&ch->lock);
            return -2;  //peer closed, no more messages
        }
        //sleep until woken (by message arrival or peer closed)
        spinlock_irq_release(&ch->lock);
        thread_sleep(&ch->waiters[my_id]);
        spinlock_irq_acquire(&ch->lock);
        
        //if peer closed while we were sleeping, return error
        if (ch->closed[1 - my_id] && !ch->queue[my_id]) {
            spinlock_irq_release(&ch->lock);
            return -2;
        }
    }
    
    //dequeue message
    channel_msg_entry_t *entry = ch->queue[my_id];
    ch->queue[my_id] = entry->next;
    if (!ch->queue[my_id]) {
        ch->queue_tail[my_id] = NULL;
    }
    ch->queue_len[my_id]--;
    spinlock_irq_release(&ch->lock);
    
    //copy data to caller (outside lock - entry is ours now)
    msg->data = entry->data;  //caller takes ownership
    msg->data_len = entry->data_len;
    msg->sender_pid = entry->sender_pid;
    entry->data = NULL;  //don't free it
    
    //grant handles to receiver
    if (entry->object_count > 0) {
        msg->handles = kzalloc(entry->object_count * sizeof(int32));
        if (!msg->handles) {
            //cleanup objects
            for (uint32 i = 0; i < entry->object_count; i++) {
                if (entry->objects[i]) object_deref(entry->objects[i]);
            }
            kfree(entry->objects);
            kfree(entry->rights);
            kfree(entry);
            return -1;
        }
        msg->handle_count = entry->object_count;
        
        for (uint32 i = 0; i < entry->object_count; i++) {
            int h = process_grant_handle(proc, entry->objects[i], entry->rights[i]);
            if (h < 0) {
                //partial failure close already-granted handles
                for (uint32 j = 0; j < i; j++) {
                    process_close_handle(proc, msg->handles[j]);
                }
                //deref remaining objects
                for (uint32 j = i; j < entry->object_count; j++) {
                    if (entry->objects[j]) object_deref(entry->objects[j]);
                }
                kfree(msg->handles);
                msg->handles = NULL;
                msg->handle_count = 0;
                kfree(entry->objects);
                kfree(entry->rights);
                kfree(entry);
                return -1;
            }
            msg->handles[i] = h;
            object_deref(entry->objects[i]);  //grant added ref sp we remove ours
        }
        
        kfree(entry->objects);
        kfree(entry->rights);
    } else {
        msg->handles = NULL;
        msg->handle_count = 0;
    }
    
    kfree(entry);
    return 0;
}

int channel_try_recv(process_t *proc, int32 endpoint_handle, channel_msg_t *msg) {
    if (!proc || !msg) return -1;
    
    channel_endpoint_t *ep = channel_get_endpoint(proc, endpoint_handle);
    if (!ep) return -1;
    
    channel_t *ch = ep->channel;
    int my_id = ep->endpoint_id;
    
    spinlock_irq_acquire(&ch->lock);
    
    //check if queue is empty
    if (!ch->queue[my_id]) {
        //check if peer closed
        if (ch->closed[1 - my_id]) {
            spinlock_irq_release(&ch->lock);
            return -2;  //peer closed, no more messages
        }
        spinlock_irq_release(&ch->lock);
        return -3;  //queue empty (non-blocking)
    }
    
    //dequeue message
    channel_msg_entry_t *entry = ch->queue[my_id];
    ch->queue[my_id] = entry->next;
    if (!ch->queue[my_id]) {
        ch->queue_tail[my_id] = NULL;
    }
    ch->queue_len[my_id]--;
    spinlock_irq_release(&ch->lock);
    
    //copy data to caller (outside lock - entry is ours now)
    msg->data = entry->data;  //caller takes ownership
    msg->data_len = entry->data_len;
    msg->sender_pid = entry->sender_pid;
    entry->data = NULL;  //don't free it
    
    //grant handles to receiver
    if (entry->object_count > 0) {
        msg->handles = kzalloc(entry->object_count * sizeof(int32));
        if (!msg->handles) {
            //cleanup objects
            for (uint32 i = 0; i < entry->object_count; i++) {
                if (entry->objects[i]) object_deref(entry->objects[i]);
            }
            kfree(entry->objects);
            kfree(entry->rights);
            kfree(entry);
            return -1;
        }
        msg->handle_count = entry->object_count;
        
        for (uint32 i = 0; i < entry->object_count; i++) {
            int h = process_grant_handle(proc, entry->objects[i], entry->rights[i]);
            if (h < 0) {
                //partial failure close already-granted handles
                for (uint32 j = 0; j < i; j++) {
                    process_close_handle(proc, msg->handles[j]);
                }
                //deref remaining objects
                for (uint32 j = i; j < entry->object_count; j++) {
                    if (entry->objects[j]) object_deref(entry->objects[j]);
                }
                kfree(msg->handles);
                msg->handles = NULL;
                msg->handle_count = 0;
                kfree(entry->objects);
                kfree(entry->rights);
                kfree(entry);
                return -1;
            }
            msg->handles[i] = h;
            object_deref(entry->objects[i]);  //grant added ref sp we remove ours
        }
        
        kfree(entry->objects);
        kfree(entry->rights);
    } else {
        msg->handles = NULL;
        msg->handle_count = 0;
    }
    
    kfree(entry);
    return 0;
}

int channel_close(process_t *proc, int32 endpoint_handle) {
    //just close the handle - the object close handler does the work
    return process_close_handle(proc, endpoint_handle);
}

int channel_peer_closed(process_t *proc, int32 endpoint_handle) {
    channel_endpoint_t *ep = channel_get_endpoint(proc, endpoint_handle);
    if (!ep) return -1;
    
    channel_t *ch = ep->channel;
    int peer_id = 1 - ep->endpoint_id;
    
    return ch->closed[peer_id];
}

//channel server functions (for kernel-side handlers)
void channel_set_handler(channel_endpoint_t *ep, 
                         void (*handler)(channel_endpoint_t *, channel_msg_t *, void *),
                         void *ctx) {
    if (!ep) return;
    ep->handler = handler;
    ep->handler_ctx = ctx;
}

void channel_clear_handler(channel_endpoint_t *ep) {
    if (!ep) return;
    ep->handler = NULL;
    ep->handler_ctx = NULL;
}

int channel_reply(channel_endpoint_t *ep, channel_msg_t *msg) {
    if (!ep || !msg) return -1;
    
    channel_t *ch = ep->channel;
    int peer_id = 1 - ep->endpoint_id;
    
    //allocate queue entry outside the lock
    channel_msg_entry_t *entry = kzalloc(sizeof(channel_msg_entry_t));
    if (!entry) return -1;
    
    //copy data
    if (msg->data_len > 0 && msg->data) {
        entry->data = kmalloc(msg->data_len);
        if (!entry->data) {
            kfree(entry);
            return -1;
        }
        memcpy(entry->data, msg->data, msg->data_len);
        entry->data_len = msg->data_len;
    }
    
    //handle transfer: copy objects from kernel-side fields
    if (msg->object_count > 0 && msg->objects && msg->rights) {
        entry->objects = kzalloc(msg->object_count * sizeof(object_t *));
        entry->rights = kzalloc(msg->object_count * sizeof(handle_rights_t));
        if (!entry->objects || !entry->rights) {
            if (entry->objects) kfree(entry->objects);
            if (entry->rights) kfree(entry->rights);
            if (entry->data) kfree(entry->data);
            kfree(entry);
            return -1;
        }
        
        entry->object_count = msg->object_count;
        for (uint32 i = 0; i < msg->object_count; i++) {
            entry->objects[i] = msg->objects[i];
            entry->rights[i] = msg->rights[i];
            object_ref(msg->objects[i]);  //add ref for the transfer
        }
    } else {
        entry->objects = NULL;
        entry->rights = NULL;
        entry->object_count = 0;
    }
    
    //--- lock for queue manipulation ---
    spinlock_irq_acquire(&ch->lock);
    
    //check if peer is closed
    if (ch->closed[peer_id]) {
        spinlock_irq_release(&ch->lock);
        //cleanup
        if (entry->data) kfree(entry->data);
        if (entry->objects) {
            for (uint32 i = 0; i < entry->object_count; i++) {
                if (entry->objects[i]) object_deref(entry->objects[i]);
            }
            kfree(entry->objects);
        }
        if (entry->rights) kfree(entry->rights);
        kfree(entry);
        return -2;
    }
    
    //enqueue to peer
    entry->next = NULL;
    if (ch->queue_tail[peer_id]) {
        ch->queue_tail[peer_id]->next = entry;
    } else {
        ch->queue[peer_id] = entry;
    }
    ch->queue_tail[peer_id] = entry;
    ch->queue_len[peer_id]++;
    
    spinlock_irq_release(&ch->lock);
    
    return 0;
}
