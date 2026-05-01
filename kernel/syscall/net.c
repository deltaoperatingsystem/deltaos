#include <syscall/syscall.h>
#include <net/net.h>
#include <net/tcp.h>
#include <net/dns.h>
#include <net/socket.h>
#include <lib/io.h>
#include <lib/string.h>
#include <proc/process.h>
#include <mm/kheap.h>
#include <arch/percpu.h>

#include <arch/mmu.h>

int copy_user_bytes(const void *user_ptr, void *kernel_buf, size len) {
    if (!user_ptr || !kernel_buf) return -1;

    uintptr start = (uintptr)user_ptr;
    if (start < USER_SPACE_START || start >= USER_SPACE_END) return -1;
    if (len > (size)(USER_SPACE_END - start)) return -1;

    percpu_t *cpu = percpu_get();
    if (mmu_copy_from_user(kernel_buf, user_ptr, len, &cpu->recovery_rip, (uintptr)mmu_user_access_fault) != 0) {
        return -1;
    }

    return 0;
}

int copy_user_cstr(const char *user_str, char *kernel_buf, size kernel_len) {
    if (!user_str || !kernel_buf || kernel_len == 0) return -1;
    if ((uintptr)user_str < USER_SPACE_START || (uintptr)user_str >= USER_SPACE_END) return -1;

    percpu_t *cpu = percpu_get();
    size i = 0;
    while (i + 1 < kernel_len) {
        uintptr addr = (uintptr)&user_str[i];
        if (addr >= USER_SPACE_END) break;

        char c;
        if (mmu_copy_from_user(&c, &user_str[i], 1, &cpu->recovery_rip, (uintptr)mmu_user_access_fault) != 0) {
            break;
        }
        
        kernel_buf[i++] = c;
        if (c == '\0') return 0;
    }

    kernel_buf[i] = '\0';
    return -1;
}

intptr sys_tcp_connect(uint32 family, const void *dst_addr, uint32 addr_len, uint16 port) {
    if (port == 0) return -1;

    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;

    net_addr_t dst = {0};
    if (family == NET_ADDR_FAMILY_IPV4) {
        if (!dst_addr || addr_len != 4) return -1;
        if (copy_user_bytes(dst_addr, &dst.addr.ipv4, 4) != 0) return -1;
        dst.family = NET_ADDR_FAMILY_IPV4;
    } else if (family == NET_ADDR_FAMILY_IPV6) {
        if (!dst_addr || addr_len != NET_IPV6_ADDR_LEN) return -1;
        if (copy_user_bytes(dst_addr, dst.addr.ipv6, NET_IPV6_ADDR_LEN) != 0) return -1;
        dst.family = NET_ADDR_FAMILY_IPV6;
    } else {
        return -1;
    }

    tcp_conn_t *conn = tcp_connect_addr(nif, &dst, port, 0);
    if (!conn) return -1;

    return (intptr)socket_object_create(conn);
}

intptr sys_tcp_listen(uint32 family, const void *local_addr, uint32 addr_len, uint16 port) {
    if (port == 0) return -1;

    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;

    net_addr_t local = {0};
    if (family == NET_ADDR_FAMILY_IPV4) {
        local.family = NET_ADDR_FAMILY_IPV4;
        if (local_addr) {
            if (addr_len != 4) return -1;
            if (copy_user_bytes(local_addr, &local.addr.ipv4, 4) != 0) return -1;
        }
    } else if (family == NET_ADDR_FAMILY_IPV6) {
        local.family = NET_ADDR_FAMILY_IPV6;
        if (local_addr) {
            if (addr_len != NET_IPV6_ADDR_LEN) return -1;
            if (copy_user_bytes(local_addr, local.addr.ipv6, NET_IPV6_ADDR_LEN) != 0) return -1;
        }
    } else {
        return -1;
    }

    tcp_conn_t *listener = tcp_listen_addr(nif, &local, port);
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
