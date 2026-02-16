#include <net/udp.h>
#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>

#define UDP_MAX_BINDS 16

typedef struct {
    uint16 port;
    udp_recv_cb_t callback;
    bool active;
} udp_bind_entry_t;

static udp_bind_entry_t udp_binds[UDP_MAX_BINDS];
static spinlock_t udp_lock = SPINLOCK_INIT;

int udp_bind(uint16 port, udp_recv_cb_t callback) {
    spinlock_acquire(&udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!udp_binds[i].active) {
            udp_binds[i].port = port;
            udp_binds[i].callback = callback;
            udp_binds[i].active = true;
            spinlock_release(&udp_lock);
            return 0;
        }
    }
    spinlock_release(&udp_lock);
    return -1;
}

void udp_unbind(uint16 port) {
    spinlock_acquire(&udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_binds[i].active && udp_binds[i].port == port) {
            udp_binds[i].active = false;
            break;
        }
    }
    spinlock_release(&udp_lock);
}

void udp_recv(netif_t *nif, uint32 src_ip, uint32 dst_ip, void *data, size len) {
    (void)dst_ip;
    if (len < sizeof(udp_header_t)) return;
    
    udp_header_t *udp = (udp_header_t *)data;
    uint16 dst_port = ntohs(udp->dst_port);
    uint16 src_port = ntohs(udp->src_port);
    uint16 udp_len = ntohs(udp->length);
    
    if (udp_len > len) return;
    
    void *payload = (uint8 *)data + sizeof(udp_header_t);
    size payload_len = udp_len - sizeof(udp_header_t);
    
    //dispatch to bound handler
    spinlock_acquire(&udp_lock);
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (udp_binds[i].active && udp_binds[i].port == dst_port) {
            udp_recv_cb_t cb = udp_binds[i].callback;
            spinlock_release(&udp_lock);
            cb(nif, src_ip, src_port, payload, payload_len);
            return;
        }
    }
    spinlock_release(&udp_lock);
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
    udp->checksum = 0;  //optional in IPv4
    
    memcpy(packet + sizeof(udp_header_t), payload, payload_len);
    
    return ipv4_send(nif, dst_ip, IPPROTO_UDP, packet, total);
}
