#include <net/arp.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <arch/cpu.h>
#include <arch/timer.h>

#define ARP_CACHE_SIZE 32

typedef struct {
    uint32 ip;
    uint8  mac[6];
    bool   valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static spinlock_irq_t arp_lock = {SPINLOCK_INIT, 0};

static void arp_cache_update(uint32 ip, const uint8 *mac) {
    spinlock_irq_acquire(&arp_lock);
    
    //check if entry already exists
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            spinlock_irq_release(&arp_lock);
            return;
        }
    }
    
    //find empty slot
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = true;
            spinlock_irq_release(&arp_lock);
            return;
        }
    }
    
    //cache full - overwrite first entry (simple eviction)
    arp_cache[0].ip = ip;
    memcpy(arp_cache[0].mac, mac, 6);
    arp_cache[0].valid = true;
    spinlock_irq_release(&arp_lock);
}

static bool arp_cache_lookup(uint32 ip, uint8 *mac_out) {
    spinlock_irq_acquire(&arp_lock);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            spinlock_irq_release(&arp_lock);
            return true;
        }
    }
    spinlock_irq_release(&arp_lock);
    return false;
}

static void arp_send_reply(netif_t *nif, uint32 dst_ip, const uint8 *dst_mac) {
    arp_header_t reply;
    reply.htype = htons(ARP_HTYPE_ETHERNET);
    reply.ptype = htons(ARP_PTYPE_IPV4);
    reply.hlen = 6;
    reply.plen = 4;
    reply.oper = htons(ARP_OP_REPLY);
    memcpy(reply.sha, nif->mac, 6);
    reply.spa = nif->ip_addr;
    memcpy(reply.tha, dst_mac, 6);
    reply.tpa = dst_ip;
    
    ethernet_send(nif, dst_mac, ETH_TYPE_ARP, &reply, sizeof(reply));
}

static void arp_send_request(netif_t *nif, uint32 target_ip) {
    arp_header_t req;
    req.htype = htons(ARP_HTYPE_ETHERNET);
    req.ptype = htons(ARP_PTYPE_IPV4);
    req.hlen = 6;
    req.plen = 4;
    req.oper = htons(ARP_OP_REQUEST);
    memcpy(req.sha, nif->mac, 6);
    req.spa = nif->ip_addr;
    memset(req.tha, 0, 6);
    req.tpa = target_ip;
    
    ethernet_send(nif, ETH_BROADCAST, ETH_TYPE_ARP, &req, sizeof(req));
}

void arp_recv(netif_t *nif, void *data, size len) {
    if (len < sizeof(arp_header_t)) return;
    
    arp_header_t *arp = (arp_header_t *)data;
    
    if (ntohs(arp->htype) != ARP_HTYPE_ETHERNET) return;
    if (ntohs(arp->ptype) != ARP_PTYPE_IPV4) return;
    
    //always learn from ARP packets
    arp_cache_update(arp->spa, arp->sha);
    
    uint16 op = ntohs(arp->oper);
    
    if (op == ARP_OP_REQUEST) {
        //is it asking for our IP?
        if (arp->tpa == nif->ip_addr) {
            printf("[arp] Request for our IP from ");
            net_print_ip(arp->spa);
            printf(", replying\n");
            arp_send_reply(nif, arp->spa, arp->sha);
        }
    } else if (op == ARP_OP_REPLY) {
        printf("[arp] Reply: ");
        net_print_ip(arp->spa);
        printf(" is ");
        net_print_mac(arp->sha);
        printf("\n");
    }
}

int arp_resolve(netif_t *nif, uint32 ip, uint8 *mac_out) {
    //broadcast addresses don't need ARP
    if (ip == 0xFFFFFFFF || ip == (nif->ip_addr | ~nif->subnet_mask)) {
        memset(mac_out, 0xFF, 6);
        return 0;
    }
    
    //check cache first
    if (arp_cache_lookup(ip, mac_out)) return 0;
    
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000; //fallback
    
    //send ARP request and poll for response
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_send_request(nif, ip);
        
        //wait up to 500ms using the system timer
        uint64 start = arch_timer_get_ticks();
        uint32 timeout_ticks = freq / 2;
        
        //safety count to prevent permanent lockup if timer is stuck
        for (volatile int safety = 0; safety < 50000000; safety++) {
            if (arp_cache_lookup(ip, mac_out)) return 0;
            if (arch_timer_get_ticks() - start >= timeout_ticks) break;
            arch_pause();
        }
    }
    
    printf("[arp] Failed to resolve ");
    net_print_ip(ip);
    printf("\n");
    return -1;
}

void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
}
