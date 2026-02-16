#include <syscall/syscall.h>
#include <lib/io.h>
#include <arch/timer.h>
#include <net/net.h>
#include <net/icmp.h>
#include <net/dns.h>
#include <proc/process.h>

intptr sys_debug_write(const char *buf, size count) {
    debug_write(buf, count);
    return (intptr)count;
}

intptr sys_get_ticks(void) {
    return (intptr)arch_timer_get_ticks();
}

intptr sys_object_get_info(handle_t h, uint32 topic, void *ptr, size len) {
    if (!handle_has_rights(h, HANDLE_RIGHT_GET_INFO)) return -1;
    
    object_t *obj = handle_get(h);
    if (!obj) return -1;
    
    intptr ret = object_get_info(obj, topic, ptr, len);
    return ret;
}

intptr sys_ping(uint32 ip_a, uint32 ip_b, uint32 ip_c, uint32 ip_d, uint32 count) {
    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;
    
    uint32 dst_ip = ip_make((uint8)ip_a, (uint8)ip_b, (uint8)ip_c, (uint8)ip_d);
    
    if (count == 0) count = 1;
    if (count > 100) count = 100;  //cap at 100
    
    for (uint32 i = 0; i < count; i++) {
        int res = icmp_send_echo(nif, dst_ip, 0x4F53, i + 1, "DeltaOS", 7);
        if (res != 0) return -(intptr)(i + 1);
    }
    
    return (intptr)count;
}

intptr sys_dns_resolve(const char *hostname, uint32 *ip_out) {
    if (!hostname || !ip_out) return -1;
    
    //validate user pointers
    if ((uintptr)hostname < USER_SPACE_START || (uintptr)hostname >= USER_SPACE_END) return -1;
    if ((uintptr)ip_out < USER_SPACE_START || (uintptr)ip_out >= USER_SPACE_END) return -1;
    
    uint32 ip;
    if (dns_resolve(hostname, &ip) != 0) return -1;
    
    *ip_out = ip;
    return 0;
}
