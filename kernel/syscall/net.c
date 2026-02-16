#include <syscall/syscall.h>
#include <net/net.h>
#include <net/tcp.h>
#include <net/dns.h>
#include <net/socket.h>
#include <lib/io.h>
#include <lib/string.h>
#include <proc/process.h>
#include <mm/kheap.h>

intptr sys_tcp_connect(const char *hostname, uint16 port) {
    if (!hostname || port == 0) return -1;
    
    //validate pointer
    if ((uintptr)hostname < USER_SPACE_START || (uintptr)hostname >= USER_SPACE_END) return -1;
    
    //safely copy to kernel buffer
    char k_hostname[256];
    size i = 0;
    while (i < 255) {
        uintptr addr = (uintptr)&hostname[i];
        if (addr >= USER_SPACE_END) return -1; 
        
        char c = hostname[i];
        k_hostname[i] = c;
        if (c == '\0') break;
        i++;
    }
    k_hostname[255] = '\0';
    if (i == 255 && k_hostname[255] != '\0') return -1; //too long
    
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

intptr sys_tcp_listen(uint16 port) {
    if (port == 0) return -1;
    
    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;
    
    tcp_conn_t *listener = tcp_listen(nif, port);
    if (!listener) return -1;
    
    handle_t h = socket_object_create(listener);
    return (intptr)h;
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
