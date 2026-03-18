#include <net/ndp.h>
#include <net/icmpv6.h>
#include <net/ipv6.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <arch/cpu.h>
#include <arch/timer.h>

#define NDP_CACHE_SIZE            32
#define NDP_OPT_SOURCE_LLADDR     1
#define NDP_OPT_TARGET_LLADDR     2
#define NDP_NA_FLAG_SOLICITED     0x40000000u
#define NDP_NA_FLAG_OVERRIDE      0x20000000u

typedef struct {
    uint8 ip[NET_IPV6_ADDR_LEN];
    uint8 mac[MAC_ADDR_LEN];
    bool  valid;
} ndp_entry_t;

typedef struct __attribute__((packed)) {
    uint8  type;
    uint8  code;
    uint16 checksum;
    uint32 reserved;
    uint8  target[NET_IPV6_ADDR_LEN];
} ndp_ns_t;

typedef struct __attribute__((packed)) {
    uint8  type;
    uint8  code;
    uint16 checksum;
    uint32 flags;
    uint8  target[NET_IPV6_ADDR_LEN];
} ndp_na_t;

typedef struct __attribute__((packed)) {
    uint8 type;
    uint8 len;
    uint8 addr[MAC_ADDR_LEN];
} ndp_lladdr_opt_t;

static ndp_entry_t ndp_cache[NDP_CACHE_SIZE];
static spinlock_irq_t ndp_lock = SPINLOCK_IRQ_INIT;

static void ndp_cache_update(const uint8 ip[NET_IPV6_ADDR_LEN],
                             const uint8 mac[MAC_ADDR_LEN]) {
    irq_state_t flags = spinlock_irq_acquire(&ndp_lock);

    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].valid && ipv6_addr_equal(ndp_cache[i].ip, ip)) {
            memcpy(ndp_cache[i].mac, mac, MAC_ADDR_LEN);
            spinlock_irq_release(&ndp_lock, flags);
            return;
        }
    }

    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (!ndp_cache[i].valid) {
            memcpy(ndp_cache[i].ip, ip, NET_IPV6_ADDR_LEN);
            memcpy(ndp_cache[i].mac, mac, MAC_ADDR_LEN);
            ndp_cache[i].valid = true;
            spinlock_irq_release(&ndp_lock, flags);
            return;
        }
    }

    memcpy(ndp_cache[0].ip, ip, NET_IPV6_ADDR_LEN);
    memcpy(ndp_cache[0].mac, mac, MAC_ADDR_LEN);
    ndp_cache[0].valid = true;
    spinlock_irq_release(&ndp_lock, flags);
}

static bool ndp_cache_lookup(const uint8 ip[NET_IPV6_ADDR_LEN], uint8 mac_out[MAC_ADDR_LEN]) {
    irq_state_t flags = spinlock_irq_acquire(&ndp_lock);
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].valid && ipv6_addr_equal(ndp_cache[i].ip, ip)) {
            memcpy(mac_out, ndp_cache[i].mac, MAC_ADDR_LEN);
            spinlock_irq_release(&ndp_lock, flags);
            return true;
        }
    }
    spinlock_irq_release(&ndp_lock, flags);
    return false;
}

static const ndp_lladdr_opt_t *ndp_find_lladdr_option(const void *opt_data, size opt_len,
                                                       uint8 opt_type) {
    const uint8 *ptr = (const uint8 *)opt_data;
    const uint8 *end = ptr + opt_len;

    while (ptr + 2 <= end) {
        uint8 type = ptr[0];
        uint8 len_units = ptr[1];
        size len = (size)len_units * 8;
        if (len == 0 || ptr + len > end) break;
        if (type == opt_type && len >= sizeof(ndp_lladdr_opt_t)) {
            return (const ndp_lladdr_opt_t *)ptr;
        }
        ptr += len;
    }

    return NULL;
}

static void ndp_send_advert(netif_t *nif, const uint8 dst[NET_IPV6_ADDR_LEN]) {
    uint8 packet[sizeof(ndp_na_t) + sizeof(ndp_lladdr_opt_t)];
    ndp_na_t *na = (ndp_na_t *)packet;
    ndp_lladdr_opt_t *opt = (ndp_lladdr_opt_t *)(packet + sizeof(ndp_na_t));
    size total = sizeof(packet);

    na->type = ICMPV6_TYPE_NEIGHBOR_ADVERT;
    na->code = 0;
    na->checksum = 0;
    na->flags = htonl(NDP_NA_FLAG_SOLICITED | NDP_NA_FLAG_OVERRIDE);
    memcpy(na->target, nif->ipv6_addr, NET_IPV6_ADDR_LEN);

    opt->type = NDP_OPT_TARGET_LLADDR;
    opt->len = 1;
    memcpy(opt->addr, nif->mac, MAC_ADDR_LEN);

    na->checksum = ipv6_upper_checksum(nif->ipv6_addr, dst,
                                       IPPROTO_ICMPV6, packet, total);
    ipv6_send_ex(nif, dst, IPPROTO_ICMPV6, 255, packet, total);
}

static void ndp_send_solicit(netif_t *nif, const uint8 target[NET_IPV6_ADDR_LEN]) {
    uint8 multicast[NET_IPV6_ADDR_LEN];
    uint8 packet[sizeof(ndp_ns_t) + sizeof(ndp_lladdr_opt_t)];
    ndp_ns_t *ns = (ndp_ns_t *)packet;
    ndp_lladdr_opt_t *opt = (ndp_lladdr_opt_t *)(packet + sizeof(ndp_ns_t));
    size total = sizeof(packet);

    ipv6_make_solicited_node_multicast(target, multicast);

    ns->type = ICMPV6_TYPE_NEIGHBOR_SOLICIT;
    ns->code = 0;
    ns->checksum = 0;
    ns->reserved = 0;
    memcpy(ns->target, target, NET_IPV6_ADDR_LEN);

    opt->type = NDP_OPT_SOURCE_LLADDR;
    opt->len = 1;
    memcpy(opt->addr, nif->mac, MAC_ADDR_LEN);

    ns->checksum = ipv6_upper_checksum(nif->ipv6_addr, multicast,
                                       IPPROTO_ICMPV6, packet, total);
    ipv6_send_ex(nif, multicast, IPPROTO_ICMPV6, 255, packet, total);
}

void ndp_recv(netif_t *nif, const uint8 src[NET_IPV6_ADDR_LEN],
              const uint8 dst[NET_IPV6_ADDR_LEN], uint8 hop_limit,
              const void *data, size len) {
    if (hop_limit != 255 || len < sizeof(icmpv6_header_t)) return;

    const icmpv6_header_t *icmp = (const icmpv6_header_t *)data;

    if (icmp->type == ICMPV6_TYPE_NEIGHBOR_SOLICIT) {
        if (len < sizeof(ndp_ns_t)) return;
        const ndp_ns_t *ns = (const ndp_ns_t *)data;
        const ndp_lladdr_opt_t *opt = ndp_find_lladdr_option(
            (const uint8 *)data + sizeof(ndp_ns_t), len - sizeof(ndp_ns_t),
            NDP_OPT_SOURCE_LLADDR);

        if (opt && !ipv6_addr_is_unspecified(src)) {
            ndp_cache_update(src, opt->addr);
        }
        if (!ipv6_addr_equal(ns->target, nif->ipv6_addr)) return;
        if (ipv6_addr_is_unspecified(src)) return;

        printf("[ndp] Neighbor solicitation from ");
        net_print_ipv6(src);
        printf(", replying\n");
        ndp_send_advert(nif, src);
    } else if (icmp->type == ICMPV6_TYPE_NEIGHBOR_ADVERT) {
        if (len < sizeof(ndp_na_t)) return;
        const ndp_na_t *na = (const ndp_na_t *)data;
        const ndp_lladdr_opt_t *opt = ndp_find_lladdr_option(
            (const uint8 *)data + sizeof(ndp_na_t), len - sizeof(ndp_na_t),
            NDP_OPT_TARGET_LLADDR);
        if (!opt) return;

        ndp_cache_update(na->target, opt->addr);
        printf("[ndp] Neighbor advertisement: ");
        net_print_ipv6(na->target);
        printf(" is ");
        net_print_mac(opt->addr);
        printf("\n");
    }

    (void)dst;
}

int ndp_resolve(netif_t *nif, const uint8 target[NET_IPV6_ADDR_LEN],
                uint8 mac_out[MAC_ADDR_LEN]) {
    if (ipv6_addr_equal(target, nif->ipv6_addr)) {
        memcpy(mac_out, nif->mac, MAC_ADDR_LEN);
        return 0;
    }
    if (target[0] == 0xFF) {
        ipv6_multicast_to_mac(target, mac_out);
        return 0;
    }
    if (ndp_cache_lookup(target, mac_out)) {
        return 0;
    }

    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;

    for (int attempt = 0; attempt < 3; attempt++) {
        ndp_send_solicit(nif, target);

        uint64 start = arch_timer_get_ticks();
        uint32 timeout_ticks = freq / 2;

        while (arch_timer_get_ticks() - start < timeout_ticks) {
            if (ndp_cache_lookup(target, mac_out)) return 0;
            arch_pause();
        }
    }

    printf("[ndp] Failed to resolve ");
    net_print_ipv6(target);
    printf("\n");
    return -1;
}