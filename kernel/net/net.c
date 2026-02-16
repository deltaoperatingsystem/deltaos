#include <net/net.h>
#include <net/ethernet.h>
#include <net/icmp.h>
#include <net/dhcp.h>
#include <lib/io.h>
#include <lib/string.h>

static netif_t *netif_list = NULL;

void net_register_netif(netif_t *nif) {
    nif->next = netif_list;
    netif_list = nif;
    
    printf("[net] Interface %s registered, MAC: ", nif->name);
    net_print_mac(nif->mac);
    printf("\n");
}

netif_t *net_get_default_netif(void) {
    return netif_list;
}

void net_rx(netif_t *nif, void *data, size len) {
    ethernet_recv(nif, data, len);
}

void net_init(void) {
    printf("[net] Networking subsystem initialized\n");
    
    //run DHCP on the default interface
    netif_t *nif = net_get_default_netif();
    if (nif && nif->ip_addr == 0) {
        dhcp_init(nif);
    }
    
    //print final IP config
    if (nif) {
        printf("[net] %s configured: ", nif->name);
        net_print_ip(nif->ip_addr);
        printf("\n");
    }
}

void net_print_mac(const uint8 *mac) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void net_print_ip(uint32 ip) {
    uint8 *b = (uint8 *)&ip;
    printf("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void net_test(void) {
    netif_t *nif = net_get_default_netif();
    if (!nif) {
        printf("[net] No network interface available for test\n");
        return;
    }
    
    if (nif->gateway == 0) {
        printf("[net] No gateway configured, skipping ping test\n");
        return;
    }
    
    printf("[net] Sending ping to gateway ");
    net_print_ip(nif->gateway);
    printf("...\n");
    
    icmp_send_echo(nif, nif->gateway, 1, 1, "DeltaOS", 7);
}
