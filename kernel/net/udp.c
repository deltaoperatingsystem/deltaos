#include <net/udp.h>
#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>

#define UDP_MAX_BINDS 16

typedef struct __attribute__((packed)) {
    uint32 src_ip;
    uint32 dst_ip;
    uint8  zero;
    uint8  protocol;
    uint16 udp_len;
} udp_pseudoheader_t;

typedef struct {
    uint16 port;
    udp_recv_cb_t callback;
    void *ctx;
    bool active;
} udp_bind_entry_t;

static udp_bind_entry_t udp_binds[UDP_MAX_BINDS];
static spinlock_irq_t udp_lock = SPINLOCK_IRQ_INIT;

static uint16 udp_checksum_ipv4(netif_t *nif, uint32 dst_ip, const udp_header_t *udp,
                                const void *payload, size payload_len) {
    udp_pseudoheader_t pseudo;
    pseudo.src_ip = nif->ip_addr;
    pseudo.dst_ip = dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = IPPROTO_UDP;
    pseudo.udp_len = udp->length;

    uint8 buf[sizeof(udp_pseudoheader_t) + ETH_MTU];
    size udp_len = sizeof(udp_header_t) + payload_len;
    memcpy(buf, &pseudo, sizeof(pseudo));
    memcpy(buf + sizeof(pseudo), udp, sizeof(udp_header_t));
    memcpy(buf + sizeof(pseudo) + sizeof(udp_header_t), payload, payload_len);

    uint16 cksum = ipv4_checksum(buf, sizeof(pseudo) + udp_len);
    return cksum == 0 ? 0xFFFF : cksum;
}

int udp_bind(uint16 port, udp_recv_cb_t callback, void *ctx) {
    irq_state_t flags = spinlock_irq_acquire(&udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!udp_binds[i].active) {
            udp_binds[i].port = port;
            udp_binds[i].callback = callback;
            udp_binds[i].ctx = ctx;
            udp_binds[i].active = true;
            spinlock_irq_release(&udp_lock, flags);
            return 0;
        }
    }
    spinlock_irq_release(&udp_lock, flags);
    return -1;
}

void udp_unbind(uint16 port) {
    irq_state_t flags = spinlock_irq_acquire(&udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_binds[i].active && udp_binds[i].port == port) {
            udp_binds[i].active = false;
            break;
        }
    }
    spinlock_irq_release(&udp_lock, flags);
}

void udp_recv(netif_t *nif, uint32 src_ip, uint32 dst_ip, void *data, size len) {
    (void)dst_ip;
    if (len < sizeof(udp_header_t)) return;
    
    udp_header_t *udp = (udp_header_t *)data;
    uint16 dst_port = ntohs(udp->dst_port);
    uint16 src_port = ntohs(udp->src_port);
    uint16 udp_len = ntohs(udp->length);
    
    if (udp_len < sizeof(udp_header_t) || udp_len > len) {
        printf("[udp] Dropped packet: udp_len=%u, len=%u\n", udp_len, (uint32)len);
        return;
    }
    
    void *payload = (uint8 *)data + sizeof(udp_header_t);
    size payload_len = (size)udp_len - sizeof(udp_header_t);
    
    //dispatch to bound handler
    irq_state_t flags = spinlock_irq_acquire(&udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_binds[i].active && udp_binds[i].port == dst_port) {
            udp_recv_cb_t cb = udp_binds[i].callback;
            void *ctx = udp_binds[i].ctx;
            spinlock_irq_release(&udp_lock, flags);
            cb(nif, src_ip, src_port, payload, payload_len, ctx);
            return;
        }
    }
    spinlock_irq_release(&udp_lock, flags);
}

int udp_send(netif_t *nif, uint32 dst_ip, uint16 src_port, uint16 dst_port,
             const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    size total = sizeof(udp_header_t) + payload_len;
    if (total > ETH_MTU) return -1;

    udp_header_t *udp = (udp_header_t *)packet;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(total);
    udp->checksum = 0;
    memcpy(packet + sizeof(udp_header_t), payload, payload_len);
    udp->checksum = udp_checksum_ipv4(nif, dst_ip, udp, payload, payload_len);

    return ipv4_send(nif, dst_ip, IPPROTO_UDP, packet, total);
}

int udp_send_no_checksum(netif_t *nif, uint32 dst_ip, uint16 src_port, uint16 dst_port,
                         const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    size total = sizeof(udp_header_t) + payload_len;
    if (total > ETH_MTU) return -1;
    
    udp_header_t *udp = (udp_header_t *)packet;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(total);
    udp->checksum = 0;
    memcpy(packet + sizeof(udp_header_t), payload, payload_len);
    
    return ipv4_send(nif, dst_ip, IPPROTO_UDP, packet, total);
}
