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
        if (copy_user_bytes(data, kbuf, len) != 0) {
            kfree(kbuf);
            return -1;
        }
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

//copy handles from user before acquiring the channel lock
intptr sys_channel_send_msg(handle_t ep, const void *data, size len,
                            const int32 *handles, uint32 handle_count) {
    if ((!data && len > 0) || handle_count > CHANNEL_MAX_MSG_HANDLES ||
        (handle_count > 0 && !handles)) {
        return -1;
    }
    if (len > CHANNEL_MAX_MSG_SIZE) return -2;

    process_t *proc = process_current();
    if (!proc) return -1;

    void *kbuf = NULL;
    if (len > 0) {
        kbuf = kmalloc(len);
        if (!kbuf) return -3;
        if (copy_user_bytes(data, kbuf, len) != 0) {
            kfree(kbuf);
            return -1;
        }
    }

    int32 khandles[CHANNEL_MAX_MSG_HANDLES];
    if (handle_count > 0 &&
        copy_user_bytes(handles, khandles, handle_count * sizeof(khandles[0])) != 0) {
        if (kbuf) kfree(kbuf);
        return -1;
    }

    channel_msg_t msg = {
        .data = kbuf,
        .data_len = len,
        .handles = handle_count ? khandles : NULL,
        .handle_count = handle_count,
    };
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
        if (!buf || copy_to_user_bytes(buf, msg.data, to_copy) != 0) {
            if (msg.data) kfree(msg.data);
            for (uint32 i = 0; i < msg.handle_count; i++) {
                process_close_handle(proc, msg.handles[i]);
            }
            if (msg.handles) kfree(msg.handles);
            return -1;
        }
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
        if (copy_to_user_bytes(data_buf, msg.data, to_copy) != 0) {
            if (msg.data) kfree(msg.data);
            for (uint32 i = 0; i < msg.handle_count; i++) {
                process_close_handle(proc, msg.handles[i]);
            }
            if (msg.handles) kfree(msg.handles);
            return -1;
        }
    }
    
    uint32 handles_to_copy = msg.handle_count < handles_len ? msg.handle_count : handles_len;
    if (handles_to_copy > 0 && msg.handles && handles_buf) {
        if (copy_to_user_bytes(handles_buf, msg.handles, handles_to_copy * sizeof(int32)) != 0) {
            if (msg.data) kfree(msg.data);
            for (uint32 i = 0; i < msg.handle_count; i++) {
                process_close_handle(proc, msg.handles[i]);
            }
            if (msg.handles) kfree(msg.handles);
            return -1;
        }
    }
    
    for (uint32 i = handles_to_copy; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    
    channel_recv_result_t k_res;
    k_res.data_len = msg.data_len;
    k_res.handle_count = msg.handle_count;
    k_res.sender_pid = msg.sender_pid;
    if (copy_to_user_bytes(result_out, &k_res, sizeof(k_res)) != 0) {
        if (msg.data) kfree(msg.data);
        for (uint32 i = 0; i < handles_to_copy; i++) {
            process_close_handle(proc, msg.handles[i]);
        }
        if (msg.handles) kfree(msg.handles);
        return -1;
    }
    
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
        if (!buf || copy_to_user_bytes(buf, msg.data, to_copy) != 0) {
            if (msg.data) kfree(msg.data);
            for (uint32 i = 0; i < msg.handle_count; i++) {
                process_close_handle(proc, msg.handles[i]);
            }
            if (msg.handles) kfree(msg.handles);
            return -1;
        }
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
        if (copy_to_user_bytes(data_buf, msg.data, to_copy) != 0) {
            if (msg.data) kfree(msg.data);
            for (uint32 i = 0; i < msg.handle_count; i++) {
                process_close_handle(proc, msg.handles[i]);
            }
            if (msg.handles) kfree(msg.handles);
            return -1;
        }
    }
    
    uint32 handles_to_copy = msg.handle_count < handles_len ? msg.handle_count : handles_len;
    if (handles_to_copy > 0 && msg.handles && handles_buf) {
        if (copy_to_user_bytes(handles_buf, msg.handles, handles_to_copy * sizeof(int32)) != 0) {
            if (msg.data) kfree(msg.data);
            for (uint32 i = 0; i < msg.handle_count; i++) {
                process_close_handle(proc, msg.handles[i]);
            }
            if (msg.handles) kfree(msg.handles);
            return -1;
        }
    }
    
    for (uint32 i = handles_to_copy; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    
    channel_recv_result_t k_res;
    k_res.data_len = msg.data_len;
    k_res.handle_count = msg.handle_count;
    k_res.sender_pid = msg.sender_pid;
    if (copy_to_user_bytes(result_out, &k_res, sizeof(k_res)) != 0) {
        if (msg.data) kfree(msg.data);
        for (uint32 i = 0; i < handles_to_copy; i++) {
            process_close_handle(proc, msg.handles[i]);
        }
        if (msg.handles) kfree(msg.handles);
        return -1;
    }
    
    if (msg.data) kfree(msg.data);
    if (msg.handles) kfree(msg.handles);
    
    return 0;
}
