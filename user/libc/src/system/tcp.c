#include <system.h>
#include <sys/syscall.h>

handle_t tcp_connect(const char *hostname, uint16 port) {
    return (handle_t)__syscall2(SYS_TCP_CONNECT, (long)hostname, (long)port);
}

handle_t tcp_connect_ipv6(const uint8 addr[IPV6_ADDR_LEN], uint16 port) {
    return (handle_t)__syscall2(SYS_TCP_CONNECT_IPV6, (long)addr, (long)port);
}

handle_t tcp_listen(uint16 port) {
    return (handle_t)__syscall1(SYS_TCP_LISTEN, (long)port);
}

handle_t tcp_listen_ipv6(uint16 port) {
    return (handle_t)__syscall1(SYS_TCP_LISTEN_IPV6, (long)port);
}

handle_t tcp_accept(handle_t listener) {
    return (handle_t)__syscall1(SYS_TCP_ACCEPT, (long)listener);
}
