#include <sys/syscall.h>
#include <system.h>

int dns_resolve(const char *hostname, uint32 *ip_out) {
    return (int)__syscall2(SYS_DNS_RESOLVE, (long)hostname, (long)ip_out);
}

int dns_resolve_aaaa(const char *hostname, uint8 ipv6_out[16]) {
    return (int)__syscall2(SYS_DNS_RESOLVE_AAAA, (long)hostname, (long)ipv6_out);
}

int get_cmdline(char *buf, size len) {
    if (!buf || len == 0) return -1;

    handle_t sys = get_obj(INVALID_HANDLE, "$devices/system", RIGHT_GET_INFO);
    if (sys == INVALID_HANDLE) return -1;

    int ret = object_get_info(sys, OBJ_INFO_BOOT_CMDLINE, buf, len);
    handle_close(sys);
    return ret;
}
