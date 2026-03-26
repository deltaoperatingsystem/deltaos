#include <io.h>
#include <string.h>
#include <system.h>

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_ipv4_literal(const char *str, uint8 out[4]) {
    int parts[4] = {0};
    int idx = 0;
    const char *p = str;
    bool saw_digit = false;

    while (*p && idx < 4) {
        if (*p >= '0' && *p <= '9') {
            int digit = (*p - '0');
            if (parts[idx] > (255 - digit) / 10) return -1;
            parts[idx] = parts[idx] * 10 + digit;
            saw_digit = true;
        } else if (*p == '.') {
            if (!saw_digit) return -1;
            idx++;
            saw_digit = false;
        } else {
            return -1;
        }
        p++;
    }

    if (idx != 3 || !saw_digit) return -1;
    for (int i = 0; i < 4; i++) {
        if (parts[i] > 255) return -1;
        out[i] = (uint8)parts[i];
    }
    return 0;
}

static int parse_ipv6_literal(const char *str, uint8 out[IPV6_ADDR_LEN]) {
    if (!str || !out) return -1;
    uint16 words[8] = {0};
    int word_count = 0;
    int compress_at = -1;
    const char *start = str;
    const char *end = str + strlen(str);
    if (start < end && *start == '[') {
        if (end <= start + 1 || end[-1] != ']') return -1;
        start++;
        end--;
    }
    if (start == end) return -1;

    if (*start == ':') {
        if (start + 1 >= end || start[1] != ':') return -1;
        compress_at = 0;
        start += 2;
        if (start == end) {
            memset(out, 0, IPV6_ADDR_LEN);
            return 0;
        }
    }

    const char *p = start;
    while (p < end) {
        uint16 value = 0;
        int digits = 0;

        if (word_count >= 8) return -1;

        while (p < end && *p != ':') {
            int hex = hex_value(*p++);
            if (hex < 0 || digits >= 4) return -1;
            value = (uint16)((value << 4) | (uint16)hex);
            digits++;
        }

        if (digits == 0) return -1;
        words[word_count++] = value;

        if (p == end) break;
        p++;

        if (p < end && *p == ':') {
            if (compress_at != -1) return -1;
            compress_at = word_count;
            p++;
            if (p == end) break;
        }
    }

    if (compress_at == -1) {
        if (word_count != 8) return -1;
    } else {
        int zeros = 8 - word_count;
        if (zeros <= 0) return -1;
        for (int i = word_count - 1; i >= compress_at; i--) {
            words[i + zeros] = words[i];
        }
        for (int i = 0; i < zeros; i++) {
            words[compress_at + i] = 0;
        }
    }

    for (int i = 0; i < 8; i++) {
        out[i * 2] = (uint8)(words[i] >> 8);
        out[i * 2 + 1] = (uint8)words[i];
    }

    return 0;
}

static handle_t connect_addr(uint8 family, const void *addr, uint32 addr_len) {
    return tcp_connect(family, addr, addr_len, 80);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: wget <hostname-or-ipv6> [path]\n");
        printf("Example: wget [fe80::1234:56ff:fe78:9abc] /\n");
        return 1;
    }
    
    const char *hostname = argv[1];
    const char *path = (argc >= 3) ? argv[2] : "/";
    uint8 ipv6_addr[IPV6_ADDR_LEN];
    bool use_ipv6 = (strchr(hostname, ':') != NULL) || (hostname[0] == '[');
    
    printf("Connecting to %s:80...\n", hostname);

    handle_t sock;
    if (use_ipv6) {
        if (parse_ipv6_literal(hostname, ipv6_addr) != 0) {
            printf("Error: invalid IPv6 literal '%s'\n", hostname);
            return 1;
        }
        sock = connect_addr(NET_ADDR_FAMILY_IPV6, ipv6_addr, sizeof(ipv6_addr));
    } else {
        uint8 ipv4_addr[4];
        if (parse_ipv4_literal(hostname, ipv4_addr) == 0) {
            sock = connect_addr(NET_ADDR_FAMILY_IPV4, ipv4_addr, sizeof(ipv4_addr));
        } else {
            uint32 ip4;
            uint8 ipv6_tmp[IPV6_ADDR_LEN];
            if (dns_resolve_aaaa(hostname, ipv6_tmp) == 0) {
                sock = connect_addr(NET_ADDR_FAMILY_IPV6, ipv6_tmp, sizeof(ipv6_tmp));
                if (sock < 0 && dns_resolve(hostname, &ip4) == 0) {
                    sock = connect_addr(NET_ADDR_FAMILY_IPV4, &ip4, sizeof(ip4));
                }
            } else if (dns_resolve(hostname, &ip4) == 0) {
                sock = connect_addr(NET_ADDR_FAMILY_IPV4, &ip4, sizeof(ip4));
            } else {
                printf("Error: failed to resolve %s\n", hostname);
                return 1;
            }
        }
    }

    if (sock < 0) {
        printf("Error: failed to connect to %s\n", hostname);
        return 1;
    }
    
    printf("Connected! Sending GET %s\n", path);
    
    //build HTTP request
    char request[512];
    int reqlen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, hostname);
    
    if (reqlen < 0 || reqlen >= (int)sizeof(request)) {
        printf("Error: request too long or formatting error\n");
        handle_close(sock);
        return 1;
    }
    
    if (handle_write(sock, request, reqlen) < 0) {
        printf("Error: failed to send request\n");
        handle_close(sock);
        return 1;
    }
    
    //read and print response
    char buf[1024];
    int n;
    int status = 0;
    bool saw_data = false;
    while ((n = handle_read(sock, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
        saw_data = true;
    }

    if (n < 0) {
        if (saw_data) {
            status = 0;
        } else {
            printf("Error: timed out waiting for HTTP response\n");
            status = 1;
        }
    }
    
    printf("\n");
    handle_close(sock);
    return status;
}
