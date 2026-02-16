#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>

static uint16 ip_id_counter = 0;

uint16 ipv4_checksum(const void *data, size len) {
    const uint16 *ptr = (const uint16 *)data;
    uint32 sum = 0;
    
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    
    //handle odd byte
    if (len == 1) {
        sum += *(const uint8 *)ptr;
    }
    
    //fold 32-bit sum into 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16)~sum;
}

void ipv4_recv(netif_t *nif, void *data, size len) {
    if (len < IPV4_HEADER_MIN_LEN) return;
    
    ipv4_header_t *ip = (ipv4_header_t *)data;
    
    //check version
    uint8 version = (ip->ver_ihl >> 4) & 0xF;
    if (version != 4) return;
    
    uint8 ihl = ip->ver_ihl & 0xF;
    size header_len = ihl * 4;
    if (header_len < IPV4_HEADER_MIN_LEN) return;
    
    //verify total length
    uint16 total_len = ntohs(ip->total_len);
    if (total_len > len) return;
    
    //verify checksum
    uint16 saved_cksum = ip->checksum;
    ip->checksum = 0;
    uint16 computed = ipv4_checksum(ip, header_len);
    ip->checksum = saved_cksum;
    if (computed != saved_cksum) {
        printf("[ipv4] Bad checksum (got 0x%04x, expected 0x%04x)\n", computed, saved_cksum);
        return;
    }
    
    //check if packet is for us (or broadcast OR we're unconfigured)
    if (ip->dst_ip != nif->ip_addr && ip->dst_ip != 0xFFFFFFFF && nif->ip_addr != 0) {
        return;
    }
    
    void *payload = (uint8 *)data + header_len;
    size payload_len = total_len - header_len;
    
    switch (ip->protocol) {
        case IPPROTO_ICMP:
            icmp_recv(nif, ip->src_ip, payload, payload_len);
            break;
        case IPPROTO_UDP:
            udp_recv(nif, ip->src_ip, ip->dst_ip, payload, payload_len);
            break;
        case IPPROTO_TCP:
            tcp_recv(nif, ip->src_ip, ip->dst_ip, payload, payload_len);
            break;
        default:
            break;
    }
}

int ipv4_send(netif_t *nif, uint32 dst_ip, uint8 protocol,
              const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    
    if (payload_len + IPV4_HEADER_MIN_LEN > ETH_MTU) return -1;
    
    ipv4_header_t *ip = (ipv4_header_t *)packet;
    ip->ver_ihl = 0x45;  //version 4, IHL 5 (20 bytes)
    ip->tos = 0;
    ip->total_len = htons(IPV4_HEADER_MIN_LEN + payload_len);
    ip->id = htons(ip_id_counter++);
    ip->flags_frag = htons(0x4000);  //don't fragment
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_ip = nif->ip_addr;
    ip->dst_ip = dst_ip;
    
    ip->checksum = ipv4_checksum(ip, IPV4_HEADER_MIN_LEN);
    
    memcpy(packet + IPV4_HEADER_MIN_LEN, payload, payload_len);
    
    //determine next-hop: if on same subnet, use dst_ip directly; otherwise gateway
    uint32 next_hop = dst_ip;
    if ((dst_ip & nif->subnet_mask) != (nif->ip_addr & nif->subnet_mask)) {
        next_hop = nif->gateway;
    }
    
    //resolve MAC via ARP
    uint8 dst_mac[6];
    if (arp_resolve(nif, next_hop, dst_mac) != 0) {
        return -1;
    }
    
    return ethernet_send(nif, dst_mac, ETH_TYPE_IPV4,
                         packet, IPV4_HEADER_MIN_LEN + payload_len);
}
