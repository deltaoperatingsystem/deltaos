#ifndef NET_ETHERNET_H
#define NET_ETHERNET_H

#include <arch/types.h>
#include <net/net.h>

#define ETH_HEADER_LEN  14
#define ETH_ALEN        6
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HEADER_LEN + ETH_MTU)

//EtherTypes
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806

typedef struct __attribute__((packed)) {
    uint8  dst[ETH_ALEN];
    uint8  src[ETH_ALEN];
    uint16 ethertype; //big-endian
} eth_header_t;

//broadcast MAC
extern const uint8 ETH_BROADCAST[ETH_ALEN];

//receive an ethernet frame (called from net_rx)
void ethernet_recv(netif_t *nif, void *data, size len);

//send an ethernet frame
int ethernet_send(netif_t *nif, const uint8 *dst_mac, uint16 ethertype,
                  const void *payload, size payload_len);

#endif
