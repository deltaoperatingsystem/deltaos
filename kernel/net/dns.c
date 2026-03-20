#include <net/dns.h>
#include <net/net.h>
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

//separate context for AAAA queries so it doesn't interfere with dns_ctx
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
        //uint32 ttl = ntohl(*(uint32 *)(ptr + 4));
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
        
        while (arch_timer_get_ticks() - start < timeout_ticks) {
            net_poll();
            if (dns_ctx.done) goto done;
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

int dns_resolve_aaaa(const char *hostname, uint8 *ipv6_out) {
    netif_t *nif = net_get_default_netif();
    if (!nif || nif->dns_server == 0) return -1;

    memset(&dns_ctx, 0, sizeof(dns_ctx));
    dns_ctx.id = (uint16)(arch_timer_get_ticks() ^ 0xAAAA);

    //build DNS AAAA query packet
    uint8 buf[512];
    memset(buf, 0, sizeof(buf));

    dns_header_t *dns = (dns_header_t *)buf;
    dns->id = htons(dns_ctx.id);
    dns->flags = htons(DNS_FLAG_RD);
    dns->qdcount = htons(1);

    //encode hostname
    uint8 *ptr = buf + sizeof(dns_header_t);
    dns_format_name(ptr, hostname);
    while (*ptr) ptr += (*ptr + 1);
    ptr++; //skip final null byte

    *(uint16 *)ptr = htons(DNS_TYPE_AAAA);
    *(uint16 *)(ptr + 2) = htons(DNS_CLASS_IN);
    ptr += 4;

    size pkt_len = (size)(ptr - buf);

    //bind a source port for the reply
    uint16 src_port = 50000 + ((dns_ctx.id ^ 0x5A5A) % 10000);
    if (udp_bind(src_port, dns_recv_handler) < 0) return -1;

    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;

    //we borrow the existing dns_ctx; dns_recv_handler stores resolved_ip
    //for AAAA we repurpose resolved_ip to hold the first 4 bytes temporarily
    //(see below) — actually we need a separate 16-byte buffer. We embed it
    //in the recv handler by scanning answers ourselves after the shared ctx signals done.
    //Simpler: re-parse the raw reply ourselves for AAAA.
    //To avoid modifying dns_ctx, we do a lightweight parallel approach:
    //the existing handler will fire; we check dns_ctx.done and then manually
    //re-parse. However the handler only extracts A records and sets error on
    //non-A answers. We must avoid the handler setting error=true for AAAA.
    //Solution: use a separate transaction ID range so both won't collide, and
    //temporarily replace with a raw recv that extracts AAAA directly.
    //
    //Cleanest fix: unbind, parse manually.
    udp_unbind(src_port);

    //-- inline AAAA resolver (raw UDP recv with custom handler) --
    static uint8  aaaa_ipv6[16];
    static bool   aaaa_done;
    static bool   aaaa_ok;
    static uint16 aaaa_id;
    aaaa_done = false; aaaa_ok = false; aaaa_id = dns_ctx.id;

    //local callback that parses AAAA answers
    void aaaa_recv(netif_t *_nif, uint32 _src_ip, uint16 _src_port,
                   const void *data2, size len2) {
        (void)_nif; (void)_src_ip; (void)_src_port;
        if (len2 < sizeof(dns_header_t)) return;
        const dns_header_t *d = (const dns_header_t *)data2;
        if (ntohs(d->id) != aaaa_id) return;
        uint16 flags = ntohs(d->flags);
        if (!(flags & DNS_FLAG_QR)) return;
        if ((flags & DNS_FLAG_RCODE) != 0) { aaaa_done = true; return; }
        uint16 qdc = ntohs(d->qdcount);
        uint16 anc = ntohs(d->ancount);
        if (anc == 0) { aaaa_done = true; return; }
        const uint8 *p2 = (const uint8 *)data2 + sizeof(dns_header_t);
        const uint8 *e2 = (const uint8 *)data2 + len2;
        for (int i2 = 0; i2 < qdc; i2++) { p2 = dns_skip_name(p2, e2); p2 += 4; }
        for (int i2 = 0; i2 < anc; i2++) {
            p2 = dns_skip_name(p2, e2);
            if (p2 + 10 > e2) break;
            uint16 rtype  = ntohs(*(uint16 *)p2);
            uint16 rclass = ntohs(*(uint16 *)(p2 + 2));
            uint16 rdlen  = ntohs(*(uint16 *)(p2 + 8));
            p2 += 10;
            if (rtype == DNS_TYPE_AAAA && rclass == DNS_CLASS_IN && rdlen == 16 && p2 + 16 <= e2) {
                memcpy(aaaa_ipv6, p2, 16);
                aaaa_ok = true; aaaa_done = true; return;
            }
            p2 += rdlen;
        }
        aaaa_done = true;
    }

    src_port = 50001 + ((dns_ctx.id ^ 0xBEEF) % 10000);
    if (udp_bind(src_port, aaaa_recv) < 0) return -1;

    for (int attempt = 0; attempt < 3; attempt++) {
        aaaa_done = false; aaaa_ok = false;
        udp_send(nif, nif->dns_server, src_port, DNS_SERVER_PORT, buf, pkt_len);
        uint64 start = arch_timer_get_ticks();
        uint32 timeout_ticks = freq * 2;
        while (arch_timer_get_ticks() - start < timeout_ticks) {
            net_poll();
            if (aaaa_done) goto aaaa_done_lbl;
            arch_pause();
        }
    }

aaaa_done_lbl:
    udp_unbind(src_port);
    if (aaaa_ok) {
        memcpy(ipv6_out, aaaa_ipv6, 16);
        return 0;
    }
    return -1;
}
