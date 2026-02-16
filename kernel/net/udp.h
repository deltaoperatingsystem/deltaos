#ifndef NET_UDP_H
#define NET_UDP_H

#include <arch/types.h>
#include <net/net.h>

typedef struct __attribute__((packed)) {
    uint16 src_port;   //big-endian
    uint16 dst_port;   //big-endian
    uint16 length;     //header + data, big-endian
    uint16 checksum;   //optional in IPv4
} udp_header_t;

//UDP receive callback
typedef void (*udp_recv_cb_t)(netif_t *nif, uint32 src_ip, uint16 src_port,
                               const void *data, size len);

//bind a port to a receive callback
int udp_bind(uint16 port, udp_recv_cb_t callback);

//unbind a port
void udp_unbind(uint16 port);

//receive a UDP packet (called from IPv4 layer)
void udp_recv(netif_t *nif, uint32 src_ip, uint32 dst_ip, void *data, size len);

//send a UDP packet
int udp_send(netif_t *nif, uint32 dst_ip, uint16 src_port, uint16 dst_port,
             const void *payload, size payload_len);

#endif
