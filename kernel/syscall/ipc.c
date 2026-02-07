#include <syscall/syscall.h>
#include <ipc/channel.h>
#include <proc/process.h>
#include <mm/kheap.h>
#include <lib/string.h>

intptr sys_channel_create(int32 *ep0_out, int32 *ep1_out) {
    if (!ep0_out || !ep1_out) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    return channel_create(proc, HANDLE_RIGHTS_DEFAULT, ep0_out, ep1_out);
}

intptr sys_channel_send(handle_t ep, const void *data, size len) {
    if (!data && len > 0) return -1;
    if (len > CHANNEL_MAX_MSG_SIZE) return -2;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    void *kbuf = NULL;
    if (len > 0) {
        kbuf = kmalloc(len);
        if (!kbuf) return -3;
        memcpy(kbuf, data, len);
    }
    
    channel_msg_t msg;
    msg.data = kbuf;
    msg.data_len = len;
    msg.handles = NULL;
    msg.handle_count = 0;
    
    int result = channel_send(proc, ep, &msg);
    if (kbuf) kfree(kbuf);
    
    return result;
}

intptr sys_channel_recv(handle_t ep, void *buf, size buflen) {
    if (!buf && buflen > 0) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int result = channel_recv(proc, ep, &msg);
    if (result != 0) return result;
    
    size to_copy = msg.data_len < buflen ? msg.data_len : buflen;
    if (to_copy > 0 && msg.data) {
        memcpy(buf, msg.data, to_copy);
    }
    
    if (msg.data) kfree(msg.data);
    
    for (uint32 i = 0; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    if (msg.handles) kfree(msg.handles);
    
    return (intptr)msg.data_len;
}

intptr sys_channel_recv_msg(handle_t ep, void *data_buf, size data_len,
                                   int32 *handles_buf, uint32 handles_len,
                                   channel_recv_result_t *result_out) {
    process_t *proc = process_current();
    if (!proc) return -1;
    if (!result_out) return -1;
    
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int result = channel_recv(proc, ep, &msg);
    if (result != 0) return result;
    
    size to_copy = msg.data_len < data_len ? msg.data_len : data_len;
    if (to_copy > 0 && msg.data && data_buf) {
        memcpy(data_buf, msg.data, to_copy);
    }
    
    uint32 handles_to_copy = msg.handle_count < handles_len ? msg.handle_count : handles_len;
    if (handles_to_copy > 0 && msg.handles && handles_buf) {
        memcpy(handles_buf, msg.handles, handles_to_copy * sizeof(int32));
    }
    
    for (uint32 i = handles_to_copy; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    
    result_out->data_len = msg.data_len;
    result_out->handle_count = msg.handle_count;
    result_out->sender_pid = msg.sender_pid;
    
    if (msg.data) kfree(msg.data);
    if (msg.handles) kfree(msg.handles);
    
    return 0;
}

intptr sys_channel_try_recv(handle_t ep, void *buf, size buflen) {
    if (!buf && buflen > 0) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int result = channel_try_recv(proc, ep, &msg);
    if (result != 0) return result;
    
    size to_copy = msg.data_len < buflen ? msg.data_len : buflen;
    if (to_copy > 0 && msg.data) {
        memcpy(buf, msg.data, to_copy);
    }
    
    if (msg.data) kfree(msg.data);
    
    for (uint32 i = 0; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    if (msg.handles) kfree(msg.handles);
    
    return (intptr)msg.data_len;
}

intptr sys_channel_try_recv_msg(handle_t ep, void *data_buf, size data_len,
                                   int32 *handles_buf, uint32 handles_len,
                                   channel_recv_result_t *result_out) {
    process_t *proc = process_current();
    if (!proc) return -1;
    if (!result_out) return -1;
    
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int result = channel_try_recv(proc, ep, &msg);
    if (result != 0) return result;
    
    size to_copy = msg.data_len < data_len ? msg.data_len : data_len;
    if (to_copy > 0 && msg.data && data_buf) {
        memcpy(data_buf, msg.data, to_copy);
    }
    
    uint32 handles_to_copy = msg.handle_count < handles_len ? msg.handle_count : handles_len;
    if (handles_to_copy > 0 && msg.handles && handles_buf) {
        memcpy(handles_buf, msg.handles, handles_to_copy * sizeof(int32));
    }
    
    for (uint32 i = handles_to_copy; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    
    result_out->data_len = msg.data_len;
    result_out->handle_count = msg.handle_count;
    result_out->sender_pid = msg.sender_pid;
    
    if (msg.data) kfree(msg.data);
    if (msg.handles) kfree(msg.handles);
    
    return 0;
}
