#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <arch/types.h>
#include <net/net.h>

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

typedef struct __attribute__((packed)) {
    uint8  type;
    uint8  code;
    uint16 checksum;
    uint16 id;
    uint16 sequence;
} icmp_header_t;

//receive an ICMP packet
void icmp_recv(netif_t *nif, uint32 src_ip, void *data, size len);

//send an ICMP echo request (ping)
int icmp_send_echo(netif_t *nif, uint32 dst_ip, uint16 id, uint16 seq,
                   const void *payload, size payload_len);

#endif
