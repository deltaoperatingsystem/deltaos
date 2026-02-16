#include <net/dns.h>
#include <net/udp.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <arch/cpu.h>
#include <arch/timer.h>

static struct {
    uint16 id;
    uint32 resolved_ip;
    bool   done;
    bool   error;
} dns_ctx;

//convert "www.google.com" to "\03www\06google\03com\00"
static void dns_format_name(uint8 *dest, const char *src) {
    int lock = 0;
    int len = strlen(src);
    
    for (int i = 0; i < len; i++) {
        if (src[i] == '.') {
            *dest++ = i - lock;
            for (; lock < i; lock++) {
                *dest++ = src[lock];
            }
            lock++; //skip the
        }
    }
    
    *dest++ = len - lock;
    for (; lock < len; lock++) {
        *dest++ = src[lock];
    }
    *dest++ = 0;
}

//skip a DNS name in a packet (handles compression)
static const uint8 *dns_skip_name(const uint8 *name, const uint8 *end) {
    while (name < end) {
        uint8 len = *name;
        if ((len & 0xC0) == 0xC0) {
            //compression pointer (2 bytes)
            return name + 2;
        }
        if (len == 0) return name + 1;
        name += len + 1;
    }
    return end;
}

static void dns_recv_handler(netif_t *nif, uint32 src_ip, uint16 src_port,
                             const void *data, size len) {
    (void)nif; (void)src_ip; (void)src_port;
    
    if (len < sizeof(dns_header_t)) return;
    
    const dns_header_t *dns = (const dns_header_t *)data;
    if (ntohs(dns->id) != dns_ctx.id) return;
    
    uint16 flags = ntohs(dns->flags);
    if (!(flags & DNS_FLAG_QR)) return; //not a response
    
    if ((flags & DNS_FLAG_RCODE) != 0) {
        printf("[dns] Server returned error code %d\n", flags & DNS_FLAG_RCODE);
        dns_ctx.error = true;
        dns_ctx.done = true;
        return;
    }
    
    uint16 qdcount = ntohs(dns->qdcount);
    uint16 ancount = ntohs(dns->ancount);
    
    if (ancount == 0) {
        dns_ctx.error = true;
        dns_ctx.done = true;
        return;
    }
    
    const uint8 *ptr = (const uint8 *)data + sizeof(dns_header_t);
    const uint8 *end = (const uint8 *)data + len;
    
    //skip questions
    for (int i = 0; i < qdcount; i++) {
        ptr = dns_skip_name(ptr, end);
        ptr += 4; //skip type and class
    }
    
    //parse answers
    for (int i = 0; i < ancount; i++) {
        ptr = dns_skip_name(ptr, end);
        if (ptr + 10 > end) break;
        
        uint16 type = ntohs(*(uint16 *)ptr);
        uint16 class = ntohs(*(uint16 *)(ptr + 2));
        uint16 rdlen = ntohs(*(uint16 *)(ptr + 8));
        ptr += 10;
        
        if (type == DNS_TYPE_A && class == DNS_CLASS_IN && rdlen == 4) {
            memcpy(&dns_ctx.resolved_ip, ptr, 4);
            dns_ctx.done = true;
            return;
        }
        
        ptr += rdlen;
    }
    
    dns_ctx.error = true;
    dns_ctx.done = true;
}

int dns_resolve(const char *hostname, uint32 *ip_out) {
    netif_t *nif = net_get_default_netif();
    if (!nif || nif->dns_server == 0) return -1;
    
    memset(&dns_ctx, 0, sizeof(dns_ctx));
    dns_ctx.id = (uint16)arch_timer_get_ticks();
    
    //build DNS packet
    uint8 buf[512];
    memset(buf, 0, sizeof(buf));
    
    dns_header_t *dns = (dns_header_t *)buf;
    dns->id = htons(dns_ctx.id);
    dns->flags = htons(DNS_FLAG_RD);
    dns->qdcount = htons(1);
    
    //encode hostname
    uint8 *ptr = buf + sizeof(dns_header_t);
    dns_format_name(ptr, hostname);
    //walk past the encoded name to find its end
    while (*ptr) ptr += (*ptr + 1);
    ptr++; //skip final null byte
    
    *(uint16 *)ptr = htons(DNS_TYPE_A);
    *(uint16 *)(ptr + 2) = htons(DNS_CLASS_IN);
    ptr += 4;
    
    size pkt_len = (size)(ptr - buf);
    
    //bind random port for reply
    uint16 src_port = 50000 + (dns_ctx.id % 10000);
    if (udp_bind(src_port, dns_recv_handler) < 0) return -1;
    
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;
    
    //retry up to 3 times (first attempt may trigger ARP which takes time)
    for (int attempt = 0; attempt < 3; attempt++) {
        dns_ctx.done = false;
        dns_ctx.error = false;
        
        udp_send(nif, nif->dns_server, src_port, DNS_SERVER_PORT, buf, pkt_len);
        
        //wait up to 2 seconds
        uint64 start = arch_timer_get_ticks();
        uint32 timeout_ticks = freq * 2;
        
        //safety count to prevent permanent lockup if timer is stuck
        for (volatile int safety = 0; safety < 100000000; safety++) {
            if (dns_ctx.done) goto done;
            if (arch_timer_get_ticks() - start >= timeout_ticks) break;
            arch_pause();
        }
    }

done:
    udp_unbind(src_port);
    
    if (dns_ctx.done && !dns_ctx.error) {
        *ip_out = dns_ctx.resolved_ip;
        return 0;
    }
    
    return -1;
}

