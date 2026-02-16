#ifndef NET_ARP_H
#define NET_ARP_H

#include <arch/types.h>
#include <net/net.h>

#define ARP_HTYPE_ETHERNET  1
#define ARP_PTYPE_IPV4      0x0800
#define ARP_OP_REQUEST      1
#define ARP_OP_REPLY        2

typedef struct __attribute__((packed)) {
    uint16 htype;      //hardware type (1 = Ethernet)
    uint16 ptype;      //protocol type (0x0800 = IPv4)
    uint8  hlen;       //hardware address length (6)
    uint8  plen;       //protocol address length (4)
    uint16 oper;       //operation (1 = request, 2 = reply)
    uint8  sha[6];     //sender hardware address
    uint32 spa;        //sender protocol address
    uint8  tha[6];     //target hardware address
    uint32 tpa;        //target protocol address
} arp_header_t;

//receive an ARP packet
void arp_recv(netif_t *nif, void *data, size len);

//resolve an IP address to a MAC address (may block while sending ARP request)
//returns 0 on success and -1 on failure
int arp_resolve(netif_t *nif, uint32 ip, uint8 *mac_out);

//initialize ARP subsystem
void arp_init(void);

#endif
