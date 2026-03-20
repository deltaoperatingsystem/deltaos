#ifndef NET_NDP_H
#define NET_NDP_H

#include <arch/types.h>
#include <net/net.h>

void ndp_recv(netif_t *nif, const uint8 src[NET_IPV6_ADDR_LEN],
              const uint8 dst[NET_IPV6_ADDR_LEN], uint8 hop_limit,
              const void *data, size len);
int ndp_resolve(netif_t *nif, const uint8 target[NET_IPV6_ADDR_LEN],
                uint8 mac_out[MAC_ADDR_LEN]);

#endif