#include <net/dns.h>
#include <net/net.h>
#include <net/udp.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <proc/sched.h>

typedef struct {
    uint16 id;
    uint16 query_type;
    uint32 resolved_ip;
    uint8  resolved_ipv6[16];
    char   cname[256];
    bool   done;
    bool   error;
    bool   answer_ok;
    bool   cname_ok;
} dns_ctx_t;

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
            lock++; //skip the dot
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
            return name + 2;
        }
        if (len == 0) return name + 1;
        name += len + 1;
    }
    return end;
}

static int dns_read_name_rec(const uint8 *packet, const uint8 *end, const uint8 *name,
                             char *out, size out_len, size *out_pos, int depth) {
    if (depth > 8 || !out || out_len == 0) return -1;
    const uint8 *cur = name;
    while (cur < end) {
        uint8 len = *cur;
        if (len == 0) {
            if (*out_pos >= out_len) return -1;
            out[*out_pos] = '\0';
            return 0;
        }

        if ((len & 0xC0) == 0xC0) {
            if (cur + 1 >= end) return -1;
            uint16 off = ((uint16)(len & 0x3F) << 8) | cur[1];
            if (packet + off >= end) return -1;
            return dns_read_name_rec(packet, end, packet + off, out, out_len, out_pos, depth + 1);
        }

        cur++;
        if (cur + len > end) return -1;
        if (*out_pos != 0) {
            if (*out_pos + 1 >= out_len) return -1;
            out[(*out_pos)++] = '.';
        }
        if (*out_pos + len >= out_len) return -1;
        memcpy(out + *out_pos, cur, len);
        *out_pos += len;
        cur += len;
    }

    return -1;
}

static void dns_recv_handler(netif_t *nif, uint32 src_ip, uint16 src_port,
                             const void *data, size len, void *ctx_ptr) {
    dns_ctx_t *ctx = (dns_ctx_t *)ctx_ptr;
    if (!ctx) return;

    //only accept replies from the DNS server we queried
    if (src_ip != nif->dns_server || src_port != DNS_SERVER_PORT) {
        return;
    }
    if (len < sizeof(dns_header_t)) {
        return;
    }

    const dns_header_t *dns = (const dns_header_t *)data;
    if (ntohs(dns->id) != ctx->id) {
        return;
    }

    uint16 flags = ntohs(dns->flags);
    if (!(flags & DNS_FLAG_QR)) {
        return;
    }

    if ((flags & DNS_FLAG_RCODE) != 0) {
        ctx->error = true;
        ctx->done = true;
        return;
    }

    uint16 qdcount = ntohs(dns->qdcount);
    uint16 ancount = ntohs(dns->ancount);
    if (ancount == 0) {
        ctx->error = true;
        ctx->done = true;
        return;
    }

    const uint8 *ptr = (const uint8 *)data + sizeof(dns_header_t);
    const uint8 *end = (const uint8 *)data + len;

    for (int i = 0; i < qdcount; i++) {
        ptr = dns_skip_name(ptr, end);
        ptr += 4;
    }

    for (int i = 0; i < ancount; i++) {
        ptr = dns_skip_name(ptr, end);
        if (ptr + 10 > end) break;

        uint16 type = ntohs(*(uint16 *)ptr);
        uint16 class = ntohs(*(uint16 *)(ptr + 2));
        uint16 rdlen = ntohs(*(uint16 *)(ptr + 8));
        ptr += 10;

        if (ctx->query_type == DNS_TYPE_A) {
            if (type == DNS_TYPE_A && class == DNS_CLASS_IN && rdlen == 4 && ptr + 4 <= end) {
                memcpy(&ctx->resolved_ip, ptr, 4);
                ctx->answer_ok = true;
                ctx->done = true;
                return;
            }
        } else if (ctx->query_type == DNS_TYPE_AAAA) {
            // For AAAA we also keep track of CNAMEs so redirects can be retried.
            if (type == DNS_TYPE_AAAA && class == DNS_CLASS_IN && rdlen == 16 && ptr + 16 <= end) {
                memcpy(ctx->resolved_ipv6, ptr, 16);
                ctx->answer_ok = true;
                ctx->done = true;
                return;
            }

            if (type == DNS_TYPE_CNAME && !ctx->cname_ok) {
                size out_pos = 0;
                if (dns_read_name_rec((const uint8 *)data, end, ptr,
                                      ctx->cname, sizeof(ctx->cname), &out_pos, 0) == 0) {
                    ctx->cname_ok = true;
                }
            }
        }

        ptr += rdlen;
    }

    ctx->done = true;
}

static int dns_send_query(netif_t *nif, dns_ctx_t *ctx, const uint8 *buf, size pkt_len) {
    uint16 src_port = 50000 + (ctx->id % 10000);
    if (udp_bind(src_port, dns_recv_handler, ctx) < 0) {
        return -1;
    }

    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;

    for (int attempt = 0; attempt < 3; attempt++) {
        ctx->done = false;
        ctx->error = false;
        ctx->answer_ok = false;

        udp_send(nif, nif->dns_server, src_port, DNS_SERVER_PORT, buf, pkt_len);

        uint64 start = arch_timer_get_ticks();
        uint32 timeout_ticks = freq * 2;

        while (arch_timer_get_ticks() - start < timeout_ticks) {
            net_poll();
            if (ctx->done) goto done;
            sched_yield();
            arch_pause();
        }
    }

done:
    udp_unbind(src_port);
    return (ctx->done && !ctx->error && ctx->answer_ok) ? 0 : -1;
}

int dns_resolve(const char *hostname, uint32 *ip_out) {
    netif_t *nif = net_get_default_netif();
    if (!nif || nif->dns_server == 0) return -1;

    dns_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.id = (uint16)arch_timer_get_ticks();
    ctx.query_type = DNS_TYPE_A;

    uint8 buf[512];
    memset(buf, 0, sizeof(buf));

    dns_header_t *dns = (dns_header_t *)buf;
    dns->id = htons(ctx.id);
    dns->flags = htons(DNS_FLAG_RD);
    dns->qdcount = htons(1);

    uint8 *ptr = buf + sizeof(dns_header_t);
    dns_format_name(ptr, hostname);
    while (*ptr) ptr += (*ptr + 1);
    ptr++;

    *(uint16 *)ptr = htons(DNS_TYPE_A);
    *(uint16 *)(ptr + 2) = htons(DNS_CLASS_IN);
    ptr += 4;

    size pkt_len = (size)(ptr - buf);

    if (dns_send_query(nif, &ctx, buf, pkt_len) != 0) {
        return -1;
    }

    *ip_out = ctx.resolved_ip;
    return 0;
}

int dns_resolve_aaaa(const char *hostname, uint8 *ipv6_out) {
    netif_t *nif = net_get_default_netif();
    if (!nif || nif->dns_server == 0) return -1;
    if (!hostname || !ipv6_out) return -1;

    char query_name[256];
    strncpy(query_name, hostname, sizeof(query_name));
    query_name[sizeof(query_name) - 1] = '\0';

    for (int redirect = 0; redirect < 4; redirect++) {
        //each redirect attempt gets its own request-local context
        dns_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.id = (uint16)(arch_timer_get_ticks() ^ 0xAAAA ^ (redirect << 8));
        ctx.query_type = DNS_TYPE_AAAA;

        uint8 buf[512];
        memset(buf, 0, sizeof(buf));

        dns_header_t *dns = (dns_header_t *)buf;
        dns->id = htons(ctx.id);
        dns->flags = htons(DNS_FLAG_RD);
        dns->qdcount = htons(1);

        uint8 *ptr = buf + sizeof(dns_header_t);
        dns_format_name(ptr, query_name);
        while (*ptr) ptr += (*ptr + 1);
        ptr++;

        *(uint16 *)ptr = htons(DNS_TYPE_AAAA);
        *(uint16 *)(ptr + 2) = htons(DNS_CLASS_IN);
        ptr += 4;

        size pkt_len = (size)(ptr - buf);

        if (dns_send_query(nif, &ctx, buf, pkt_len) != 0) {
            return -1;
        }

        if (ctx.answer_ok) {
            memcpy(ipv6_out, ctx.resolved_ipv6, 16);
            return 0;
        }

        if (ctx.cname_ok && ctx.cname[0] != '\0') {
            //follow one more CNAME hop with the redirected name
            strncpy(query_name, ctx.cname, sizeof(query_name));
            query_name[sizeof(query_name) - 1] = '\0';
            continue;
        }

        return -1;
    }

    return -1;
}
