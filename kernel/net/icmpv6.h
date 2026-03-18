#ifndef NET_ICMPV6_H
#define NET_ICMPV6_H

#include <arch/types.h>
#include <net/net.h>

#define ICMPV6_TYPE_ECHO_REQUEST      128
#define ICMPV6_TYPE_ECHO_REPLY        129
#define ICMPV6_TYPE_NEIGHBOR_SOLICIT  135
#define ICMPV6_TYPE_NEIGHBOR_ADVERT   136

typedef struct __attribute__((packed)) {
    uint8  type;
    uint8  code;
    uint16 checksum;
} icmpv6_header_t;

typedef struct __attribute__((packed)) {
    uint8  type;
    uint8  code;
    uint16 checksum;
    uint16 id;
    uint16 sequence;
} icmpv6_echo_t;

void icmpv6_recv(netif_t *nif, const uint8 src[NET_IPV6_ADDR_LEN],
                 const uint8 dst[NET_IPV6_ADDR_LEN], uint8 hop_limit,
                 void *data, size len);
int icmpv6_send_echo(netif_t *nif, const uint8 dst[NET_IPV6_ADDR_LEN],
                     uint16 id, uint16 seq, const void *payload, size payload_len);

#endif