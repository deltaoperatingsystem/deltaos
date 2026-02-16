#include <system.h>
#include <io.h>
#include <string.h>

static int parse_ip(const char *str, uint8 *a, uint8 *b, uint8 *c, uint8 *d) {
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

int main(int argc, char **argv) {
    uint32 count = 4;
    const char *target = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            count = 0;
            const char *p = argv[++i];
            while (*p >= '0' && *p <= '9') {
                count = count * 10 + (*p - '0');
                p++;
            }
        } else {
            target = argv[i];
        }
    }
    
    if (!target) {
        puts("Usage: ping [-c count] <ip>\n");
        puts("Example: ping 10.0.2.2\n");
        return 1;
    }
    
    uint8 a, b, c, d;
    uint32 ip_val;
    if (parse_ip(target, &a, &b, &c, &d) == 0) {
        ip_val = (uint32)a | ((uint32)b << 8) | ((uint32)c << 16) | ((uint32)d << 24);
    } else {
        printf("Resolving %s... ", target);
        if (dns_resolve(target, &ip_val) != 0) {
            printf("failed to resolve hostname\n");
            return 1;
        }
        uint8 *p = (uint8 *)&ip_val;
        a = p[0]; b = p[1]; c = p[2]; d = p[3];
        printf("%u.%u.%u.%u\n", a, b, c, d);
    }
    
    printf("PING %u.%u.%u.%u: %u packets\n", a, b, c, d, count);
    
    int res = ping(a, b, c, d, count);
    if (res > 0) {
        printf("Sent %d ICMP echo request(s)\n", res);
    } else {
        printf("Ping failed (error %d)\n", res);
        return 1;
    }
    
    return 0;
}
