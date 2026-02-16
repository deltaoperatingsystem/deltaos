#ifndef NET_NET_H
#define NET_NET_H

#include <arch/types.h>
#include <net/netbuf.h>
#include <lib/spinlock.h>

/*
 *network interface abstraction
 *each NIC driver registers a netif_t 
 */

#define MAC_ADDR_LEN 6
#define MAX_NETIFS   4

typedef struct netif {
    char name[16];  //interface name (e.g. "eth0")
    uint8 mac[MAC_ADDR_LEN]; //MAC address
    uint32 ip_addr; //IPv4 address (network byte order)
    uint32 subnet_mask; //subnet mask (network byte order)
    uint32 gateway; //default gateway (network byte order)
    uint32 dns_server; //DNS server (network byte order)
    bool up; //interface is active
    
    //driver callbacks
    int (*send)(struct netif *nif, const void *data, size len);
    void *driver_data; //driver private data
    
    struct netif *next;
} netif_t;

//register a network interface
void net_register_netif(netif_t *nif);

//get the default (first) network interface
netif_t *net_get_default_netif(void);

//called by NIC driver when a packet is received
void net_rx(netif_t *nif, void *data, size len);

//initialize the networking subsystem
void net_init(void);

//send a test ping to the gateway
void net_test(void);
//make an IPv4 address from 4 octets (in network byte order)
static inline uint32 ip_make(uint8 a, uint8 b, uint8 c, uint8 d) {
    return (uint32)a | ((uint32)b << 8) | ((uint32)c << 16) | ((uint32)d << 24);
}

//print helpers
void net_print_mac(const uint8 *mac);
void net_print_ip(uint32 ip);

#endif
