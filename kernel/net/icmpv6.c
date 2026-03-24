#include <net/icmpv6.h>
#include <net/ipv6.h>
#include <net/ndp.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>

void icmpv6_recv(netif_t *nif, const uint8 src[NET_IPV6_ADDR_LEN],
                 const uint8 dst[NET_IPV6_ADDR_LEN], uint8 hop_limit,
                 void *data, size len) {
    if (len < sizeof(icmpv6_header_t)) return;

    icmpv6_header_t *icmp = (icmpv6_header_t *)data;
    uint16 saved_cksum = icmp->checksum;
    icmp->checksum = 0;
    uint16 computed = ipv6_upper_checksum(src, dst, IPPROTO_ICMPV6, data, len);
    icmp->checksum = saved_cksum;

    if (computed != saved_cksum) {
        printf("[icmpv6] Bad checksum (got 0x%04x, expected 0x%04x)\n",
               computed, saved_cksum);
        return;
    }

    if (icmp->type == ICMPV6_TYPE_ECHO_REQUEST && icmp->code == 0) {
        if (len < sizeof(icmpv6_echo_t) || ipv6_addr_is_unspecified(src)) return;

        printf("[icmpv6] Echo request from ");
        net_print_ipv6(src);
        printf(", replying\n");

        uint8 reply[ETH_MTU];
        if (len > ETH_MTU) return;
        memcpy(reply, data, len);

        icmpv6_echo_t *rep = (icmpv6_echo_t *)reply;
        rep->type = ICMPV6_TYPE_ECHO_REPLY;
        rep->code = 0;
        rep->checksum = 0;
        rep->checksum = ipv6_upper_checksum(nif->ipv6_addr, src,
                                            IPPROTO_ICMPV6, reply, len);
        ipv6_send(nif, src, IPPROTO_ICMPV6, reply, len);
    } else if (icmp->type == ICMPV6_TYPE_ECHO_REPLY) {
        if (len < sizeof(icmpv6_echo_t)) return;
        icmpv6_echo_t *echo = (icmpv6_echo_t *)data;
        printf("[icmpv6] Echo reply from ");
        net_print_ipv6(src);
        printf(" seq=%u\n", ntohs(echo->sequence));
    } else if (icmp->type == ICMPV6_TYPE_ROUTER_SOLICIT ||
               icmp->type == ICMPV6_TYPE_ROUTER_ADVERT ||
               icmp->type == ICMPV6_TYPE_NEIGHBOR_SOLICIT ||
               icmp->type == ICMPV6_TYPE_NEIGHBOR_ADVERT) {
        ndp_recv(nif, src, dst, hop_limit, data, len);
    }
}

int icmpv6_send_echo(netif_t *nif, const uint8 dst[NET_IPV6_ADDR_LEN],
                     uint16 id, uint16 seq, const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    size total = sizeof(icmpv6_echo_t) + payload_len;
    if (total > ETH_MTU) return -1;

    icmpv6_echo_t *icmp = (icmpv6_echo_t *)packet;
    icmp->type = ICMPV6_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(id);
    icmp->sequence = htons(seq);

    if (payload_len > 0 && payload) {
        memcpy(packet + sizeof(icmpv6_echo_t), payload, payload_len);
    }

    icmp->checksum = ipv6_upper_checksum(nif->ipv6_addr, dst,
                                         IPPROTO_ICMPV6, packet, total);
    return ipv6_send(nif, dst, IPPROTO_ICMPV6, packet, total);
}