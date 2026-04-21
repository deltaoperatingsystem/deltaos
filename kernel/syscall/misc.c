#include <syscall/syscall.h>
#include <lib/io.h>
#include <arch/timer.h>
#include <net/net.h>
#include <net/icmp.h>
#include <net/icmpv6.h>
#include <net/dns.h>
#include <proc/process.h>
#include <arch/percpu.h>
#include <errno.h>

static int write_user_byte(uint8 *ptr, uint8 value) {
    percpu_t *cpu = percpu_get();
    cpu->recovery_rip = (uintptr)&&fault;
    *ptr = value;
    cpu->recovery_rip = 0;
    return 0;
fault:
    cpu->recovery_rip = 0;
    return -EFAULT;
}

int copy_to_user_bytes(void *user_ptr, const void *kernel_buf, size len) {
    if (len == 0) return 0;
    if (!user_ptr || !kernel_buf) return -EFAULT;

    uintptr dst_addr = (uintptr)user_ptr;
    if (dst_addr < USER_SPACE_START || dst_addr >= USER_SPACE_END) return -EFAULT;
    if (len > (size)(USER_SPACE_END - dst_addr)) return -EFAULT;

    const uint8 *src = (const uint8 *)kernel_buf;
    uint8 *dst = (uint8 *)user_ptr;
    size i = 0;

    //align to word boundary if needed
    while (i < len && ((uintptr)&dst[i] & (sizeof(uintptr) - 1))) {
        if (write_user_byte(&dst[i], src[i]) != 0) return -EFAULT;
        i++;
    }

    //bulk copy with a single recovery region
    if (i + sizeof(uintptr) <= len) {
        percpu_t *cpu = percpu_get();
        cpu->recovery_rip = (uintptr)&&bulk_fault;
        
        while (i + sizeof(uintptr) <= len) {
            *(uintptr *)&dst[i] = *(const uintptr *)&src[i];
            i += sizeof(uintptr);
        }
        
        cpu->recovery_rip = 0;
    }

    //remaining tail
    while (i < len) {
        if (write_user_byte(&dst[i], src[i]) != 0) return -EFAULT;
        i++;
    }

    return 0;

bulk_fault:
    percpu_get()->recovery_rip = 0;
    return -EFAULT;
}

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

    if (topic == OBJ_INFO_BLOCK_DEVICE) {
        if (!ptr || len < sizeof(block_device_info_t)) return -1;

        block_device_info_t info = {0};
        intptr ret = object_get_info(obj, topic, &info, sizeof(info));
        if (ret < 0) return ret;
        if (copy_to_user_bytes(ptr, &info, sizeof(info)) != 0) return -EFAULT;
        return 0;
    }

    if (topic == OBJ_INFO_VT_STATE) {
        if (!ptr || len < sizeof(vt_info_t)) return -1;

        vt_info_t info = {0};
        intptr ret = object_get_info(obj, topic, &info, sizeof(info));
        if (ret < 0) return ret;
        if (copy_to_user_bytes(ptr, &info, sizeof(info)) != 0) return -EFAULT;
        return 0;
    }

    return object_get_info(obj, topic, ptr, len);
}

static intptr sys_ping_impl(uint8 family, const void *dst_addr, uint32 addr_len, uint32 count) {
    netif_t *nif = net_get_default_netif();
    if (!nif) return -1;

    if (count == 0) count = 1;
    if (count > 100) count = 100;  //cap at 100

    if (family == NET_ADDR_FAMILY_IPV4) {
        if (!dst_addr || addr_len != 4) return -1;
        uint32 dst_ip = 0;
        if (copy_user_bytes(dst_addr, &dst_ip, sizeof(dst_ip)) != 0) return -1;

        for (uint32 i = 0; i < count; i++) {
            int res = icmp_send_echo(nif, dst_ip, 0x4F53, i + 1, "DeltaOS", 7);
            if (res != 0) return -(intptr)(i + 1);
        }
        return (intptr)count;
    }

    if (family == NET_ADDR_FAMILY_IPV6) {
        if (!dst_addr || addr_len != NET_IPV6_ADDR_LEN) return -1;
        uint8 dst[NET_IPV6_ADDR_LEN];
        if (copy_user_bytes(dst_addr, dst, sizeof(dst)) != 0) return -1;

        for (uint32 i = 0; i < count; i++) {
            int res = icmpv6_send_echo(nif, dst, 0x4F53, (uint16)(i + 1), "DeltaOS", 7);
            if (res != 0) return -(intptr)(i + 1);
        }
        return (intptr)count;
    }

    return -1;
}

intptr sys_ping(uint32 family, const void *dst_addr, uint32 addr_len, uint32 count) {
    return sys_ping_impl((uint8)family, dst_addr, addr_len, count);
}

intptr sys_dns_resolve(const char *hostname, uint32 *ip_out) {
    if (!hostname || !ip_out) return -1;
    
    //validate user pointers
    if ((uintptr)hostname < USER_SPACE_START || (uintptr)hostname >= USER_SPACE_END) return -1;
    if ((uintptr)ip_out < USER_SPACE_START || (uintptr)ip_out >= USER_SPACE_END) return -1;

    char k_hostname[256];
    if (copy_user_cstr(hostname, k_hostname, sizeof(k_hostname)) != 0) return -1;
    
    uint32 ip;
    if (dns_resolve(k_hostname, &ip) != 0) return -1;
    
    if (copy_to_user_bytes(ip_out, &ip, sizeof(ip)) != 0) return -1;
    return 0;
}

intptr sys_dns_resolve_aaaa(const char *hostname, uint8 *ipv6_out) {
    if (!hostname || !ipv6_out) return -1;
    if ((uintptr)hostname < USER_SPACE_START || (uintptr)hostname >= USER_SPACE_END) return -1;
    if ((uintptr)ipv6_out < USER_SPACE_START || (uintptr)ipv6_out > USER_SPACE_END - NET_IPV6_ADDR_LEN) return -1;

    char k_hostname[256];
    if (copy_user_cstr(hostname, k_hostname, sizeof(k_hostname)) != 0) return -1;

    uint8 ipv6[NET_IPV6_ADDR_LEN];
    if (dns_resolve_aaaa(k_hostname, ipv6) != 0) return -1;

    if (copy_to_user_bytes(ipv6_out, ipv6, NET_IPV6_ADDR_LEN) != 0) return -1;
    return 0;
}
