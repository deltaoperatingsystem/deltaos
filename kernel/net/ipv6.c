#include <net/ipv6.h>
#include <net/ethernet.h>
#include <net/icmpv6.h>
#include <net/ndp.h>
#include <net/tcp.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>

static uint32 checksum_add(const void *data, size len, uint32 sum) {
    const uint16 *ptr = (const uint16 *)data;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32)(*(const uint8 *)ptr);
    }
    return sum;
}

static uint16 checksum_finish(uint32 sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16)~sum;
}

static bool ipv6_is_all_nodes_multicast(const uint8 addr[NET_IPV6_ADDR_LEN]) {
    static const uint8 all_nodes[NET_IPV6_ADDR_LEN] =
        {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    return memcmp(addr, all_nodes, sizeof(all_nodes)) == 0;
}

bool ipv6_addr_equal(const uint8 a[NET_IPV6_ADDR_LEN], const uint8 b[NET_IPV6_ADDR_LEN]) {
    return memcmp(a, b, NET_IPV6_ADDR_LEN) == 0;
}

bool ipv6_addr_is_unspecified(const uint8 addr[NET_IPV6_ADDR_LEN]) {
    static const uint8 zero[NET_IPV6_ADDR_LEN] = {0};
    return memcmp(addr, zero, sizeof(zero)) == 0;
}

void ipv6_make_link_local(uint8 out[NET_IPV6_ADDR_LEN], const uint8 mac[MAC_ADDR_LEN]) {
    memset(out, 0, NET_IPV6_ADDR_LEN);
    out[0] = 0xFE;
    out[1] = 0x80;
    out[8] = mac[0] ^ 0x02;
    out[9] = mac[1];
    out[10] = mac[2];
    out[11] = 0xFF;
    out[12] = 0xFE;
    out[13] = mac[3];
    out[14] = mac[4];
    out[15] = mac[5];
}

bool ipv6_prefix_match(const uint8 a[NET_IPV6_ADDR_LEN], const uint8 b[NET_IPV6_ADDR_LEN],
                       uint8 prefix_len) {
    uint8 full_bytes = prefix_len / 8;
    uint8 rem_bits = prefix_len % 8;

    if (full_bytes > 0 && memcmp(a, b, full_bytes) != 0) {
        return false;
    }
    if (rem_bits == 0) {
        return true;
    }

    uint8 mask = (uint8)(0xFF << (8 - rem_bits));
    return (a[full_bytes] & mask) == (b[full_bytes] & mask);
}

void ipv6_make_solicited_node_multicast(const uint8 addr[NET_IPV6_ADDR_LEN],
                                        uint8 out[NET_IPV6_ADDR_LEN]) {
    memset(out, 0, NET_IPV6_ADDR_LEN);
    out[0] = 0xFF;
    out[1] = 0x02;
    out[11] = 0x01;
    out[12] = 0xFF;
    out[13] = addr[13];
    out[14] = addr[14];
    out[15] = addr[15];
}

void ipv6_multicast_to_mac(const uint8 addr[NET_IPV6_ADDR_LEN], uint8 mac[MAC_ADDR_LEN]) {
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = addr[12];
    mac[3] = addr[13];
    mac[4] = addr[14];
    mac[5] = addr[15];
}

bool ipv6_is_for_us(netif_t *nif, const uint8 dst[NET_IPV6_ADDR_LEN]) {
    uint8 solicited[NET_IPV6_ADDR_LEN];

    if (ipv6_addr_equal(dst, nif->ipv6_addr)) {
        return true;
    }
    if (ipv6_is_all_nodes_multicast(dst)) {
        return true;
    }

    ipv6_make_solicited_node_multicast(nif->ipv6_addr, solicited);
    return ipv6_addr_equal(dst, solicited);
}

uint16 ipv6_upper_checksum(const uint8 src[NET_IPV6_ADDR_LEN],
                           const uint8 dst[NET_IPV6_ADDR_LEN],
                           uint8 next_header, const void *payload,
                           size payload_len) {
    uint32 sum = 0;
    uint32 len32 = (uint32)payload_len;
    uint8 upper_len[4] = {
        (uint8)(len32 >> 24),
        (uint8)(len32 >> 16),
        (uint8)(len32 >> 8),
        (uint8)len32,
    };
    uint8 next[4] = {0, 0, 0, next_header};

    sum = checksum_add(src, NET_IPV6_ADDR_LEN, sum);
    sum = checksum_add(dst, NET_IPV6_ADDR_LEN, sum);
    sum = checksum_add(upper_len, sizeof(upper_len), sum);
    sum = checksum_add(next, sizeof(next), sum);
    sum = checksum_add(payload, payload_len, sum);
    return checksum_finish(sum);
}

void ipv6_recv(netif_t *nif, void *data, size len) {
    if (len < IPV6_HEADER_LEN) return;

    ipv6_header_t *ip = (ipv6_header_t *)data;
    if ((((uint8 *)data)[0] >> 4) != 6) return;

    uint16 payload_len = ntohs(ip->payload_len);
    if ((size)payload_len + IPV6_HEADER_LEN > len) return;
    if (!ipv6_is_for_us(nif, ip->dst)) return;

    void *payload = (uint8 *)data + IPV6_HEADER_LEN;

    switch (ip->next_header) {
        case IPPROTO_ICMPV6:
            icmpv6_recv(nif, ip->src, ip->dst, ip->hop_limit, payload, payload_len);
            break;
        case IPPROTO_TCP:
            tcp_recv_ipv6(nif, ip->src, ip->dst, payload, payload_len);
            break;
        default:
            break;
    }
}

int ipv6_send_ex(netif_t *nif, const uint8 dst_addr[NET_IPV6_ADDR_LEN],
                 uint8 next_header, uint8 hop_limit,
                 const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    uint8 dst_mac[MAC_ADDR_LEN];
    uint8 next_hop[NET_IPV6_ADDR_LEN];

    if (ipv6_addr_is_unspecified(nif->ipv6_addr)) return -1;
    if (payload_len + IPV6_HEADER_LEN > ETH_MTU) return -1;

    ipv6_header_t *ip = (ipv6_header_t *)packet;
    ip->ver_tc_flow = htonl(6u << 28);
    ip->payload_len = htons((uint16)payload_len);
    ip->next_header = next_header;
    ip->hop_limit = hop_limit;
    memcpy(ip->src, nif->ipv6_addr, NET_IPV6_ADDR_LEN);
    memcpy(ip->dst, dst_addr, NET_IPV6_ADDR_LEN);
    memcpy(packet + IPV6_HEADER_LEN, payload, payload_len);

    if (dst_addr[0] == 0xFF) {
        ipv6_multicast_to_mac(dst_addr, dst_mac);
    } else {
        memcpy(next_hop, dst_addr, NET_IPV6_ADDR_LEN);
        if (!ipv6_prefix_match(dst_addr, nif->ipv6_addr, nif->ipv6_prefix_len)) {
            uint8 router[NET_IPV6_ADDR_LEN];
            if (ndp_get_default_router(nif, router)) {
                memcpy(next_hop, router, NET_IPV6_ADDR_LEN);
            }
        }
        if (ndp_resolve(nif, next_hop, dst_mac) != 0) {
            return -1;
        }
    }

    return ethernet_send(nif, dst_mac, ETH_TYPE_IPV6,
                         packet, IPV6_HEADER_LEN + payload_len);
}

int ipv6_send(netif_t *nif, const uint8 dst_addr[NET_IPV6_ADDR_LEN],
              uint8 next_header, const void *payload, size payload_len) {
    return ipv6_send_ex(nif, dst_addr, next_header, IPV6_HOP_LIMIT_DEFAULT,
                        payload, payload_len);
}
