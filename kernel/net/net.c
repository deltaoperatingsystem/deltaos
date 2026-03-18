#include <net/net.h>
#include <net/ethernet.h>
#include <net/icmp.h>
#include <net/dhcp.h>
#include <net/tcp.h>
#include <net/ipv6.h>
#include <drivers/rtl8139.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>

static netif_t *netif_list = NULL;
static spinlock_irq_t netif_lock = SPINLOCK_IRQ_INIT;

void net_register_netif(netif_t *nif) {
    if (ipv6_addr_is_unspecified(nif->ipv6_addr)) {
        ipv6_make_link_local(nif->ipv6_addr, nif->mac);
    }
    if (nif->ipv6_prefix_len == 0) {
        nif->ipv6_prefix_len = 64;
    }

    irq_state_t flags = spinlock_irq_acquire(&netif_lock);
    nif->next = netif_list;
    netif_list = nif;
    spinlock_irq_release(&netif_lock, flags);
    
    printf("[net] Interface %s registered, MAC: ", nif->name);
    net_print_mac(nif->mac);
    printf("\n");
    printf("[net] Interface %s IPv6 link-local: ", nif->name);
    net_print_ipv6(nif->ipv6_addr);
    printf("\n");
}

netif_t *net_get_default_netif(void) {
    irq_state_t flags = spinlock_irq_acquire(&netif_lock);
    netif_t *res = netif_list;
    spinlock_irq_release(&netif_lock, flags);
    return res;
}

void net_rx(netif_t *nif, void *data, size len) {
    ethernet_recv(nif, data, len);
}

void net_poll(void) {
    //keep RX moving even if the interrupt line is flaky or delayed
    rtl8139_poll();
}

void net_init(void) {
    printf("[net] Networking subsystem initialized\n");
    
    //initialize TCP subsystem (zero connections table)
    tcp_init();
    
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
        printf("[net] %s IPv6: ", nif->name);
        net_print_ipv6(nif->ipv6_addr);
        printf("/%u\n", nif->ipv6_prefix_len);
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

void net_print_ipv6(const uint8 *addr) {
    for (int i = 0; i < NET_IPV6_ADDR_LEN; i += 2) {
        uint16 part = ((uint16)addr[i] << 8) | addr[i + 1];
        if (i > 0) printf(":");
        printf("%x", part);
    }
}

void net_print_addr(const net_addr_t *addr) {
    if (!addr) {
        printf("<null>");
        return;
    }

    switch (addr->family) {
        case NET_ADDR_FAMILY_IPV4:
            net_print_ip(addr->addr.ipv4);
            break;
        case NET_ADDR_FAMILY_IPV6:
            net_print_ipv6(addr->addr.ipv6);
            break;
        default:
            printf("<none>");
            break;
    }
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
