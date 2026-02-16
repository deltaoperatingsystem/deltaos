#ifndef NET_IPV4_H
#define NET_IPV4_H

#include <arch/types.h>
#include <net/net.h>

#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

#define IPV4_HEADER_MIN_LEN 20

typedef struct __attribute__((packed)) {
    uint8  ver_ihl;    //version (4 bits) + IHL (4 bits)
    uint8  tos;        //type of service
    uint16 total_len;  //total length (big-endian)
    uint16 id;         //identification
    uint16 flags_frag; //flags (3 bits) + fragment offset (13 bits)
    uint8  ttl;        //time to live
    uint8  protocol;   //protocol (ICMP=1, TCP=6, UDP=17)
    uint16 checksum;   //header checksum
    uint32 src_ip;     //source IP (network byte order)
    uint32 dst_ip;     //destination IP (network byte order)
} ipv4_header_t;

//receive an IPv4 packet (called from ethernet layer)
void ipv4_recv(netif_t *nif, void *data, size len);

//send an IPv4 packet
int ipv4_send(netif_t *nif, uint32 dst_ip, uint8 protocol,
              const void *payload, size payload_len);

//compute IP checksum over a buffer
uint16 ipv4_checksum(const void *data, size len);

#endif
