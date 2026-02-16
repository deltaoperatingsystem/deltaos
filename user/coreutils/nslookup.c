#include <system.h>
#include <io.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: nslookup <hostname>\n");
        return 1;
    }
    
    const char *hostname = argv[1];
    uint32 ip;
    
    printf("Server: (kernel)\n\n");
    printf("Non-authoritative answer:\n");
    
    if (dns_resolve(hostname, &ip) < 0) {
        printf("*** Can't find %s: No answer\n", hostname);
        return 1;
    }
    
    uint8 *b = (uint8 *)&ip;
    printf("Name:    %s\n", hostname);
    printf("Address: %u.%u.%u.%u\n", b[0], b[1], b[2], b[3]);
    
    return 0;
}
