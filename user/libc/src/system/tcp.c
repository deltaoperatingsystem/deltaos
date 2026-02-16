#include <system.h>
#include <sys/syscall.h>

handle_t tcp_connect(const char *hostname, uint16 port) {
    return (handle_t)__syscall2(SYS_TCP_CONNECT, (long)hostname, (long)port);
}

handle_t tcp_listen(uint16 port) {
    return (handle_t)__syscall1(SYS_TCP_LISTEN, (long)port);
}

handle_t tcp_accept(handle_t listener) {
    return (handle_t)__syscall1(SYS_TCP_ACCEPT, (long)listener);
}
