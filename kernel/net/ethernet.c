#include <net/ethernet.h>
#include <net/endian.h>
#include <net/arp.h>
#include <net/ipv4.h>
#include <lib/io.h>
#include <lib/string.h>

const uint8 ETH_BROADCAST[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void ethernet_recv(netif_t *nif, void *data, size len) {
    if (len < ETH_HEADER_LEN) return;
    
    eth_header_t *eth = (eth_header_t *)data;
    uint16 ethertype = ntohs(eth->ethertype);
    
    void *payload = (uint8 *)data + ETH_HEADER_LEN;
    size payload_len = len - ETH_HEADER_LEN;
    
    switch (ethertype) {
        case ETH_TYPE_ARP:
            arp_recv(nif, payload, payload_len);
            break;
        case ETH_TYPE_IPV4:
            ipv4_recv(nif, payload, payload_len);
            break;
        default:
            break;
    }
}

int ethernet_send(netif_t *nif, const uint8 *dst_mac, uint16 ethertype,
                  const void *payload, size payload_len) {
    if (payload_len > ETH_MTU) return -1;
    
    uint8 frame[ETH_FRAME_MAX];
    eth_header_t *eth = (eth_header_t *)frame;
    
    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, nif->mac, ETH_ALEN);
    eth->ethertype = htons(ethertype);
    
    memcpy(frame + ETH_HEADER_LEN, payload, payload_len);
    
    size total = ETH_HEADER_LEN + payload_len;
    
    //pad to minimum ethernet frame size (60 bytes without FCS)
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    
    return nif->send(nif, frame, total);
}
