#ifndef NET_NDP_H
#define NET_NDP_H

#include <arch/types.h>
#include <net/net.h>

void ndp_recv(netif_t *nif, const uint8 src[NET_IPV6_ADDR_LEN],
              const uint8 dst[NET_IPV6_ADDR_LEN], uint8 hop_limit,
              const void *data, size len);
int ndp_resolve(netif_t *nif, const uint8 target[NET_IPV6_ADDR_LEN],
                uint8 mac_out[MAC_ADDR_LEN]);
int ndp_discover_router(netif_t *nif);
bool ndp_get_default_router(netif_t *nif, uint8 out[NET_IPV6_ADDR_LEN]);
void ndp_set_default_router(netif_t *nif, const uint8 router[NET_IPV6_ADDR_LEN]);
void ndp_clear_default_router(netif_t *nif);
void ndp_timer_tick(void);

#endif
