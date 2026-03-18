#include <syscall/syscall.h>
#include <net/net.h>
#include <net/tcp.h>
#include <net/dns.h>
#include <net/socket.h>
#include <lib/io.h>
#include <lib/string.h>
#include <proc/process.h>
#include <mm/kheap.h>

static int copy_user_bytes(const void *user_ptr, void *kernel_buf, size len) {
    if (!user_ptr || !kernel_buf) return -1;

    uintptr start = (uintptr)user_ptr;
    if (start < USER_SPACE_START || start >= USER_SPACE_END) return -1;
    if (len > (size)(USER_SPACE_END - start)) return -1;

    const uint8 *src = (const uint8 *)user_ptr;
    uint8 *dst = (uint8 *)kernel_buf;
    for (size i = 0; i < len; i++) {
        dst[i] = src[i];
    }

    return 0;
}

static int copy_user_cstr(const char *user_str, char *kernel_buf, size kernel_len) {
    if (!user_str || !kernel_buf || kernel_len == 0) return -1;
    if ((uintptr)user_str < USER_SPACE_START || (uintptr)user_str >= USER_SPACE_END) return -1;

    size i = 0;
    while (i + 1 < kernel_len) {
        uintptr addr = (uintptr)&user_str[i];
        if (addr >= USER_SPACE_END) return -1;

        char c = user_str[i];
        kernel_buf[i++] = c;
        if (c == '\0') return 0;
    }

    kernel_buf[kernel_len - 1] = '\0';
    return -1;
}

intptr sys_tcp_connect(const char *hostname, uint16 port) {
    if (!hostname || port == 0) return -1;

    char k_hostname[256];
    if (copy_user_cstr(hostname, k_hostname, sizeof(k_hostname)) != 0) return -1;
    
    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;
    
    //resolve hostname to IP
    uint32 dst_ip;
    if (dns_resolve(k_hostname, &dst_ip) != 0) {
        printf("[tcp] Failed to resolve %s\n", k_hostname);
        return -1;
    }
    
    //establish TCP connection with automatic local port selection
    tcp_conn_t *conn = tcp_connect(nif, dst_ip, port, 0);
    if (!conn) return -1;
    
    //wrap in a socket object and return handle
    handle_t h = socket_object_create(conn);
    return (intptr)h;
}

intptr sys_tcp_connect_ipv6(const uint8 *dst_ip, uint16 port) {
    if (!dst_ip || port == 0) return -1;

    uint8 k_dst_ip[NET_IPV6_ADDR_LEN];
    if (copy_user_bytes(dst_ip, k_dst_ip, sizeof(k_dst_ip)) != 0) return -1;

    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;

    tcp_conn_t *conn = tcp_connect_ipv6(nif, k_dst_ip, port, 0);
    if (!conn) return -1;

    return (intptr)socket_object_create(conn);
}

intptr sys_tcp_listen(uint16 port) {
    if (port == 0) return -1;
    
    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;
    
    tcp_conn_t *listener = tcp_listen(nif, port);
    if (!listener) return -1;
    
    handle_t h = socket_object_create(listener);
    return (intptr)h;
}

intptr sys_tcp_listen_ipv6(uint16 port) {
    if (port == 0) return -1;

    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;

    tcp_conn_t *listener = tcp_listen_ipv6(nif, port);
    if (!listener) return -1;

    return (intptr)socket_object_create(listener);
}

intptr sys_tcp_accept(handle_t listen_h) {
    object_t *obj = handle_get(listen_h);
    if (!obj) return -1;
    
    if (obj->type != OBJECT_SOCKET) {
        return -1;
    }
    
    tcp_conn_t *listener = (tcp_conn_t *)obj->data;
    if (!listener || !listener->listening) {
        return -1;
    }
    
    //explicitly ref to pin it during blocking call
    object_ref(obj);
    
    tcp_conn_t *conn = tcp_accept(listener);
    
    //release the pin
    object_deref(obj);
    
    if (!conn) return -1;
    
    handle_t h = socket_object_create(conn);
    return (intptr)h;
}
