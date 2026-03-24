#include <system.h>
#include <sys/syscall.h>

handle_t tcp_connect(uint8 family, const void *addr, uint32 addr_len, uint16 port) {
    return (handle_t)__syscall4(SYS_TCP_CONNECT, (long)family, (long)addr, (long)addr_len, (long)port);
}

handle_t tcp_listen(uint8 family, const void *addr, uint32 addr_len, uint16 port) {
    return (handle_t)__syscall4(SYS_TCP_LISTEN, (long)family, (long)addr, (long)addr_len, (long)port);
}

handle_t tcp_accept(handle_t listener) {
    return (handle_t)__syscall1(SYS_TCP_ACCEPT, (long)listener);
}
