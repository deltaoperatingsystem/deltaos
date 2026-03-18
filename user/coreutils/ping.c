#include <system.h>
#include <io.h>
#include <string.h>

//try to parse a dotted-decimal IPv4 address like "1.2.3.4"
//returns 0 on success, -1 if it doesn't look like IPv4
static int parse_ipv4(const char *str, uint8 *a, uint8 *b, uint8 *c, uint8 *d) {
    int parts[4] = {0};
    int idx = 0;
    const char *p = str;
    
    while (*p && idx < 4) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10 + (*p - '0');
        } else if (*p == '.') {
            idx++;
        } else {
            return -1;
        }
        p++;
    }
    
    if (idx != 3) return -1;
    for (int i = 0; i < 4; i++) {
        if (parts[i] > 255) return -1;
    }
    
    *a = (uint8)parts[0];
    *b = (uint8)parts[1];
    *c = (uint8)parts[2];
    *d = (uint8)parts[3];
    return 0;
}

//try to parse a bare IPv6 address like "2001:db8::1"
//supports :: compression., returns 0 on success
static int parse_ipv6(const char *str, uint8 out[16]) {
    //count colons to detect IPv6
    int colons = 0;
    for (const char *p = str; *p; p++) {
        if (*p == ':') colons++;
        else if ((*p < '0' || *p > '9') &&
                 (*p < 'a' || *p > 'f') &&
                 (*p < 'A' || *p > 'F') &&
                 *p != ':') {
            return -1; //not a hex/colon string
        }
    }
    if (colons < 2) return -1;

    uint16 groups[8] = {0};
    int    ngroups   = 0;
    int    compress  = -1; //index of :: expansion point
    const char *p = str;

    while (*p) {
        if (*p == ':' && *(p+1) == ':') {
            if (compress >= 0) return -1; //double :: not allowed
            compress = ngroups;
            p += 2;
            if (!*p) break;
            continue;
        }
        if (*p == ':') { p++; continue; }

        //parse a hex group
        uint32 val = 0;
        int digits = 0;
        while ((*p >= '0' && *p <= '9') ||
               (*p >= 'a' && *p <= 'f') ||
               (*p >= 'A' && *p <= 'F')) {
            uint8 nibble;
            if (*p >= '0' && *p <= '9') nibble = (uint8)(*p - '0');
            else if (*p >= 'a' && *p <= 'f') nibble = (uint8)(10 + *p - 'a');
            else nibble = (uint8)(10 + *p - 'A');
            val = (val << 4) | nibble;
            digits++;
            p++;
        }
        if (digits == 0 || digits > 4 || val > 0xFFFF) return -1;
        if (ngroups >= 8) return -1;
        groups[ngroups++] = (uint16)val;
        if (*p == ':') p++;
    }

    //expand :: compression
    if (compress >= 0) {
        int zeros = 8 - ngroups;
        if (zeros < 1) return -1;
        //shift groups after compress point to the right
        for (int i = ngroups - 1; i >= compress; i--) {
            groups[i + zeros] = groups[i];
        }
        for (int i = compress; i < compress + zeros; i++) {
            groups[i] = 0;
        }
    } else if (ngroups != 8) {
        return -1;
    }

    for (int i = 0; i < 8; i++) {
        out[i*2]   = (uint8)(groups[i] >> 8);
        out[i*2+1] = (uint8)(groups[i] & 0xFF);
    }
    return 0;
}

static void print_ipv6(const uint8 addr[16]) {
    for (int i = 0; i < 16; i += 2) {
        if (i > 0) printf(":");
        uint16 part = ((uint16)addr[i] << 8) | addr[i+1];
        printf("%x", part);
    }
}

int main(int argc, char **argv) {
    uint32 count = 4;
    const char *target = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            count = 0;
            const char *p = argv[++i];
            while (*p >= '0' && *p <= '9') {
                count = count * 10 + (uint32)(*p - '0');
                p++;
            }
        } else {
            target = argv[i];
        }
    }
    
    if (!target) {
        puts("Usage: ping [-c count] <host|ipv4|ipv6>\n");
        puts("Examples:\n");
        puts("  ping 10.0.2.2\n");
        puts("  ping 2001:db8::1\n");
        puts("  ping google.com\n");
        puts("  ping ipv6.google.com\n");
        return 1;
    }

    //--- try bare IPv4 first ---
    uint8 a, b, c, d;
    if (parse_ipv4(target, &a, &b, &c, &d) == 0) {
        uint32 ip_val = (uint32)a | ((uint32)b << 8) | ((uint32)c << 16) | ((uint32)d << 24);
        (void)ip_val;
        printf("PING %u.%u.%u.%u: %u packets\n", a, b, c, d, count);
        int res = ping(a, b, c, d, count);
        if (res > 0) {
            printf("Sent %d ICMP echo request(s)\n", res);
            return 0;
        }
        printf("Ping failed (error %d)\n", res);
        return 1;
    }

    //try bare Ipv6
    uint8 ipv6[16];
    if (parse_ipv6(target, ipv6) == 0) {
        printf("PING6 ");
        print_ipv6(ipv6);
        printf(": %u packets\n", count);
        int res = ping6(ipv6, count);
        if (res > 0) {
            printf("Sent %d ICMPv6 echo request(s)\n", res);
            return 0;
        }
        printf("Ping6 failed (error %d)\n", res);
        return 1;
    }

    //hostname: try A record first, then AAAA
    uint32 ip_val;
    printf("Resolving %s (A)... ", target);
    if (dns_resolve(target, &ip_val) == 0) {
        uint8 *pb = (uint8 *)&ip_val;
        a = pb[0]; b = pb[1]; c = pb[2]; d = pb[3];
        printf("%u.%u.%u.%u\n", a, b, c, d);
        printf("PING %u.%u.%u.%u: %u packets\n", a, b, c, d, count);
        int res = ping(a, b, c, d, count);
        if (res > 0) {
            printf("Sent %d ICMP echo request(s)\n", res);
            return 0;
        }
        printf("Ping failed (error %d)\n", res);
        return 1;
    }

    printf("not found\n");
    printf("Resolving %s (AAAA)... ", target);
    if (dns_resolve_aaaa(target, ipv6) == 0) {
        print_ipv6(ipv6);
        printf("\n");
        printf("PING6 ");
        print_ipv6(ipv6);
        printf(": %u packets\n", count);
        int res = ping6(ipv6, count);
        if (res > 0) {
            printf("Sent %d ICMPv6 echo request(s)\n", res);
            return 0;
        }
        printf("Ping6 failed (error %d)\n", res);
        return 1;
    }

    printf("not found\n");
    printf("ping: failed to resolve '%s'\n", target);
    return 1;
}
