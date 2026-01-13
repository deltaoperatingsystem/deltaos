#include <system.h>
#include <sys/syscall.h>

int channel_create(int32 *ep0, int32 *ep1) {
    return __syscall2(SYS_CHANNEL_CREATE, (long)ep0, (long)ep1);
}

int channel_send(int32 ep, const void *data, int len) {
    return __syscall3(SYS_CHANNEL_SEND, ep, (long)data, len);
}

int channel_recv(int32 ep, void *buf, int buflen) {
    return __syscall3(SYS_CHANNEL_RECV, ep, (long)buf, buflen);
}

int channel_try_recv(int32 ep, void *buf, int buflen) {
    return __syscall3(SYS_CHANNEL_TRY_RECV, ep, (long)buf, buflen);
}

int channel_recv_msg(int32 ep, void *data_buf, int data_len,
                     int32 *handles_buf, uint32 handles_len,
                     channel_recv_result_t *result) {
    return __syscall6(SYS_CHANNEL_RECV_MSG, ep, (long)data_buf, data_len,
                      (long)handles_buf, handles_len, (long)result);
}
