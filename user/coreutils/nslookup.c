#include <system.h>
#include <io.h>
#include <string.h>

static void print_ipv6(const uint8 addr[16]) {
    for (int i = 0; i < 16; i += 2) {
        if (i > 0) printf(":");
        uint16 part = ((uint16)addr[i] << 8) | addr[i + 1];
        printf("%x", part);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: nslookup <hostname>\n");
        return 1;
    }
    
    const char *hostname = argv[1];
    uint32 ip;
    uint8 ipv6[IPV6_ADDR_LEN];
    
    printf("Server: (kernel)\n\n");
    printf("Non-authoritative answer:\n");
    
    if (dns_resolve(hostname, &ip) == 0) {
        uint8 *b = (uint8 *)&ip;
        printf("Name:    %s\n", hostname);
        printf("Address: %u.%u.%u.%u\n", b[0], b[1], b[2], b[3]);
        return 0;
    }

    if (dns_resolve_aaaa(hostname, ipv6) == 0) {
        printf("Name:    %s\n", hostname);
        printf("Address: ");
        print_ipv6(ipv6);
        printf("\n");
        return 0;
    }

    printf("*** Can't find %s: No answer\n", hostname);
    return 1;
}
