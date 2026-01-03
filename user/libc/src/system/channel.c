#include <system.h>
#include <sys/syscall.h>

int handle_close(int32 h) {
    return __syscall1(SYS_HANDLE_CLOSE, h);
}

int channel_create(int32 *ep0, int32 *ep1) {
    return __syscall2(SYS_CHANNEL_CREATE, (long)ep0, (long)ep1);
}

int channel_send(int32 ep, const void *data, int len) {
    return __syscall3(SYS_CHANNEL_SEND, ep, (long)data, len);
}

int channel_recv(int32 ep, void *buf, int buflen) {
    return __syscall3(SYS_CHANNEL_RECV, ep, (long)buf, buflen);
}
