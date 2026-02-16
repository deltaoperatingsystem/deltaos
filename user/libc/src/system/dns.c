#include <sys/syscall.h>
#include <system.h>

int dns_resolve(const char *hostname, uint32 *ip_out) {
    return (int)__syscall2(SYS_DNS_RESOLVE, (long)hostname, (long)ip_out);
}
