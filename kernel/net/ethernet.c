#include <net/ethernet.h>
#include <net/endian.h>
#include <net/arp.h>
#include <net/ipv4.h>
#include <net/ipv6.h>
#include <lib/io.h>
#include <lib/string.h>

const uint8 ETH_BROADCAST[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool ethernet_is_valid_unicast_mac(const uint8 mac[ETH_ALEN]) {
    bool all_zero = true;

    for (size i = 0; i < ETH_ALEN; i++) {
        if (mac[i] != 0) {
            all_zero = false;
            break;
        }
    }

    return !all_zero && (mac[0] & 0x01) == 0;
}

static bool ethernet_is_valid_arp_seed_ip(uint32 ip) {
    uint8 first_octet = (uint8)(ip & 0xFF);

    if (ip == 0 || ip == 0xFFFFFFFF) {
        return false;
    }

    return (first_octet & 0xF0) != 0xE0;
}

static bool ethernet_is_valid_arp_seed_ipv4(const void *payload, size payload_len,
                                            const uint8 mac[ETH_ALEN]) {
    if (payload_len < IPV4_HEADER_MIN_LEN || !ethernet_is_valid_unicast_mac(mac)) {
        return false;
    }

    ipv4_header_t *ip = (ipv4_header_t *)payload;
    if (((ip->ver_ihl >> 4) & 0xF) != 4) {
        return false;
    }

    uint8 ihl = ip->ver_ihl & 0x0F;
    size header_len = ihl * 4;
    if (header_len < IPV4_HEADER_MIN_LEN || header_len > payload_len) {
        return false;
    }

    uint16 total_len = ntohs(ip->total_len);
    if (total_len < header_len || total_len > payload_len) {
        return false;
    }

    uint16 saved_cksum = ip->checksum;
    ip->checksum = 0;
    uint16 computed_cksum = ipv4_checksum(ip, header_len);
    ip->checksum = saved_cksum;
    if (computed_cksum != saved_cksum) {
        return false;
    }

    return ethernet_is_valid_arp_seed_ip(ip->src_ip);
}

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
            if (ethernet_is_valid_arp_seed_ipv4(payload, payload_len, eth->src)) {
                ipv4_header_t *ip = (ipv4_header_t *)payload;
                arp_seed(ip->src_ip, eth->src);
            }
            ipv4_recv(nif, payload, payload_len);
            break;
        case ETH_TYPE_IPV6:
            ipv6_recv(nif, payload, payload_len);
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
