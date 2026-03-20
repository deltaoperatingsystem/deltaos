#ifndef NET_IPV6_H
#define NET_IPV6_H

#include <arch/types.h>
#include <net/net.h>

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP    1
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP     6
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP     17
#endif
#define IPPROTO_ICMPV6  58

#define IPV6_HEADER_LEN         40
#define IPV6_HOP_LIMIT_DEFAULT  64

typedef struct __attribute__((packed)) {
    uint32 ver_tc_flow;
    uint16 payload_len;
    uint8  next_header;
    uint8  hop_limit;
    uint8  src[NET_IPV6_ADDR_LEN];
    uint8  dst[NET_IPV6_ADDR_LEN];
} ipv6_header_t;

void ipv6_recv(netif_t *nif, void *data, size len);
int ipv6_send(netif_t *nif, const uint8 dst_addr[NET_IPV6_ADDR_LEN],
              uint8 next_header, const void *payload, size payload_len);
int ipv6_send_ex(netif_t *nif, const uint8 dst_addr[NET_IPV6_ADDR_LEN],
                 uint8 next_header, uint8 hop_limit,
                 const void *payload, size payload_len);
uint16 ipv6_upper_checksum(const uint8 src[NET_IPV6_ADDR_LEN],
                           const uint8 dst[NET_IPV6_ADDR_LEN],
                           uint8 next_header, const void *payload,
                           size payload_len);
void ipv6_make_link_local(uint8 out[NET_IPV6_ADDR_LEN], const uint8 mac[MAC_ADDR_LEN]);
bool ipv6_addr_equal(const uint8 a[NET_IPV6_ADDR_LEN], const uint8 b[NET_IPV6_ADDR_LEN]);
bool ipv6_addr_is_unspecified(const uint8 addr[NET_IPV6_ADDR_LEN]);
bool ipv6_prefix_match(const uint8 a[NET_IPV6_ADDR_LEN], const uint8 b[NET_IPV6_ADDR_LEN],
                       uint8 prefix_len);
bool ipv6_is_for_us(netif_t *nif, const uint8 dst[NET_IPV6_ADDR_LEN]);
void ipv6_make_solicited_node_multicast(const uint8 addr[NET_IPV6_ADDR_LEN],
                                        uint8 out[NET_IPV6_ADDR_LEN]);
void ipv6_multicast_to_mac(const uint8 addr[NET_IPV6_ADDR_LEN], uint8 mac[MAC_ADDR_LEN]);

#endif