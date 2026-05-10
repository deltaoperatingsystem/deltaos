#include <net/net.h>
#include <net/ethernet.h>
#include <net/icmp.h>
#include <net/dhcp.h>
#include <net/ndp.h>
#include <net/tcp.h>
#include <net/ipv6.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/thread.h>

static netif_t *netif_list = NULL;
static netif_t *netif_tail = NULL;
static netif_t *default_netif = NULL;
static spinlock_irq_t netif_lock = SPINLOCK_IRQ_INIT;
static bool net_boot_started = false;

static bool netif_has_ipv6_route(const netif_t *nif) {
    return nif && !ipv6_addr_is_unspecified(nif->ipv6_gateway);
}

static bool netif_is_configured(const netif_t *nif) {
    return nif && (nif->gateway != 0 || netif_has_ipv6_route(nif));
}

static netif_t *net_pick_default_locked(void) {
    netif_t *fallback = default_netif ? default_netif : netif_list;

    if (netif_is_configured(default_netif)) {
        return default_netif;
    }

    for (netif_t *nif = netif_list; nif; nif = nif->next) {
        if (nif->gateway != 0) {
            return nif;
        }
    }

    for (netif_t *nif = netif_list; nif; nif = nif->next) {
        if (nif->ip_addr != 0) {
            return nif;
        }
    }

    for (netif_t *nif = netif_list; nif; nif = nif->next) {
        if (nif->dns_server != 0) {
            return nif;
        }
    }

    for (netif_t *nif = netif_list; nif; nif = nif->next) {
        if (netif_has_ipv6_route(nif)) {
            return nif;
        }
    }

    return fallback;
}

void net_register_netif(netif_t *nif) {
    if (ipv6_addr_is_unspecified(nif->ipv6_addr)) {
        ipv6_make_link_local(nif->ipv6_addr, nif->mac);
    }
    if (nif->ipv6_prefix_len == 0) {
        nif->ipv6_prefix_len = 64;
    }

    irq_state_t flags = spinlock_irq_acquire(&netif_lock);
    nif->next = NULL;
    if (!netif_list) {
        netif_list = nif;
        netif_tail = nif;
        if (!default_netif) default_netif = nif;
    } else {
        netif_tail->next = nif;
        netif_tail = nif;
    }
    spinlock_irq_release(&netif_lock, flags);
    
    char mac_buf[18];
    char ipv6_buf[40];
    net_format_mac(nif->mac, mac_buf, sizeof(mac_buf));
    net_format_ipv6(nif->ipv6_addr, ipv6_buf, sizeof(ipv6_buf));
    printf("[net] Interface %s registered, MAC: %s\n", nif->name, mac_buf);
    printf("[net] Interface %s IPv6 link-local: %s\n", nif->name, ipv6_buf);
}

netif_t *net_get_default_netif(void) {
    irq_state_t flags = spinlock_irq_acquire(&netif_lock);
    netif_t *res = net_pick_default_locked();
    spinlock_irq_release(&netif_lock, flags);
    return res;
}

void net_rx(netif_t *nif, void *data, size len) {
    ethernet_recv(nif, data, len);
}

void net_poll(void) {
    //keep RX moving even if the interrupt line is flaky or delayed
    netif_t *snapshot[MAX_NETIFS];
    size count = 0;

    irq_state_t flags = spinlock_irq_acquire(&netif_lock);
    for (netif_t *nif = netif_list; nif && count < MAX_NETIFS; nif = nif->next) {
        if (nif->poll) {
            snapshot[count++] = nif;
        }
    }
    spinlock_irq_release(&netif_lock, flags);

    for (size i = 0; i < count; i++) {
        snapshot[i]->poll(snapshot[i]);
    }
}

static void net_boot_worker(void *arg) {
    (void)arg;

    //probe DHCP in reverse registration order so we hit the NIC that actually leases first
    netif_t *order[MAX_NETIFS];
    size count = 0;

    irq_state_t flags = spinlock_irq_acquire(&netif_lock);
    for (netif_t *nif = netif_list; nif && count < MAX_NETIFS; nif = nif->next) {
        order[count++] = nif;
    }
    spinlock_irq_release(&netif_lock, flags);

    if (count == 0) {
        printf("[net] No network interfaces available for bring-up\n");
        flags = spinlock_irq_acquire(&netif_lock);
        net_boot_started = false;
        spinlock_irq_release(&netif_lock, flags);
        return;
    }

    bool have_primary = false;

    for (size i = count; i > 0; i--) {
        netif_t *nif = order[i - 1];

        if (nif->ip_addr == 0) {
            if (dhcp_init(nif) != 0) {
                printf("[net] %s DHCP probe failed\n", nif->name);
                continue;
            }
        }

        char ip_buf[16];
        char ipv6_buf[40];
        net_format_ip(nif->ip_addr, ip_buf, sizeof(ip_buf));
        net_format_ipv6(nif->ipv6_addr, ipv6_buf, sizeof(ipv6_buf));
        printf("[net] %s configured: %s\n", nif->name, ip_buf);
        printf("[net] %s IPv6: %s/%u\n", nif->name, ipv6_buf, nif->ipv6_prefix_len);

        if (nif->up) {
            ndp_discover_router(nif);
        }

        if (!have_primary) {
            flags = spinlock_irq_acquire(&netif_lock);
            default_netif = nif;
            spinlock_irq_release(&netif_lock, flags);
            have_primary = true;
        }
    }

    flags = spinlock_irq_acquire(&netif_lock);
    netif_t *best = net_pick_default_locked();
    if (best && best != default_netif) {
        default_netif = best;
    }
    spinlock_irq_release(&netif_lock, flags);

    net_test();

    flags = spinlock_irq_acquire(&netif_lock);
    net_boot_started = false;
    spinlock_irq_release(&netif_lock, flags);
}

void net_init(void) {
    printf("[net] Networking subsystem initialized\n");
    
    //initialize TCP subsystem (zero connections table)
    tcp_init();

    irq_state_t flags = spinlock_irq_acquire(&netif_lock);
    if (net_boot_started) {
        spinlock_irq_release(&netif_lock, flags);
        printf("[net] background bring-up already queued\n");
        return;
    }
    net_boot_started = true;
    spinlock_irq_release(&netif_lock, flags);

    process_t *kernel = process_get_kernel();
    if (!kernel) {
        printf("[net] failed to get kernel process for background bring-up\n");
        net_boot_worker(NULL);
        return;
    }

    thread_t *thread = thread_create(kernel, net_boot_worker, NULL);
    if (!thread) {
        printf("[net] failed to create background bring-up thread\n");
        net_boot_worker(NULL);
        return;
    }

    sched_add(thread);
    printf("[net] background bring-up thread scheduled\n");
}

void net_print_mac(const uint8 *mac) {
    char buf[18];
    net_format_mac(mac, buf, sizeof(buf));
    printf("%s", buf);
}

void net_print_ip(uint32 ip) {
    char buf[16];
    net_format_ip(ip, buf, sizeof(buf));
    printf("%s", buf);
}

void net_print_ipv6(const uint8 *addr) {
    char buf[40];
    net_format_ipv6(addr, buf, sizeof(buf));
    printf("%s", buf);
}

void net_format_mac(const uint8 *mac, char *buf, size buflen) {
    if (!buf || buflen == 0) return;
    snprintf(buf, buflen, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void net_format_ip(uint32 ip, char *buf, size buflen) {
    if (!buf || buflen == 0) return;
    uint8 *b = (uint8 *)&ip;
    snprintf(buf, buflen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void net_format_ipv6(const uint8 *addr, char *buf, size buflen) {
    if (!buf || buflen == 0) return;
    size pos = 0;
    buf[0] = '\0';

    for (int i = 0; i < NET_IPV6_ADDR_LEN; i += 2) {
        uint16 part = ((uint16)addr[i] << 8) | addr[i + 1];
        if (i > 0 && pos < buflen) {
            int n = snprintf(buf + pos, buflen - pos, ":");
            if (n > 0) pos += (size)n;
        }
        if (pos < buflen) {
            int n = snprintf(buf + pos, buflen - pos, "%x", part);
            if (n > 0) pos += (size)n;
        }
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
    
    char gateway_buf[16];
    net_format_ip(nif->gateway, gateway_buf, sizeof(gateway_buf));
    printf("[net] Sending ping to gateway %s...\n", gateway_buf);
    
    icmp_send_echo(nif, nif->gateway, 1, 1, "DeltaOS", 7);
}
