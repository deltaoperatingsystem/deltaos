#include <net/tcp.h>
#include <net/ipv4.h>
#include <net/ipv6.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <errno.h>
#include <mm/kheap.h>
#include <proc/thread.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <proc/sched.h>
#include <proc/event.h>

static tcp_conn_t connections[TCP_MAX_CONNECTIONS];
static spinlock_irq_t tcp_lock = SPINLOCK_IRQ_INIT;

typedef struct __attribute__((packed)) {
    uint32 src_ip;
    uint32 dst_ip;
    uint8  zero;
    uint8  protocol;
    uint16 tcp_len;
} tcp_pseudoheader_t;

static uint32 tcp_checksum_add_be(const uint8 *data, size len, uint32 sum) {
    while (len > 1) {
        sum += ((uint32)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32)data[0] << 8;
    }
    return sum;
}


static void tcp_addr_from_ipv4(net_addr_t *addr, uint32 ip) {
    addr->family = NET_ADDR_FAMILY_IPV4;
    addr->addr.ipv4 = ip;
}

static void tcp_addr_from_ipv6(net_addr_t *addr, const uint8 ip[NET_IPV6_ADDR_LEN]) {
    addr->family = NET_ADDR_FAMILY_IPV6;
    memcpy(addr->addr.ipv6, ip, NET_IPV6_ADDR_LEN);
}

static bool tcp_addr_equal(const net_addr_t *a, const net_addr_t *b) {
    if (a->family != b->family) return false;
    if (a->family == NET_ADDR_FAMILY_IPV4) {
        return a->addr.ipv4 == b->addr.ipv4;
    }
    if (a->family == NET_ADDR_FAMILY_IPV6) {
        return memcmp(a->addr.ipv6, b->addr.ipv6, NET_IPV6_ADDR_LEN) == 0;
    }
    return false;
}

static bool tcp_addr_is_unspecified(const net_addr_t *addr) {
    if (addr->family == NET_ADDR_FAMILY_IPV4) {
        return addr->addr.ipv4 == 0;
    }
    if (addr->family == NET_ADDR_FAMILY_IPV6) {
        return ipv6_addr_is_unspecified(addr->addr.ipv6);
    }
    return true;
}

static bool tcp_listener_matches(const tcp_conn_t *listener, const net_addr_t *dst_addr,
                                 uint16 dst_port) {
    if (!listener->active || !listener->listening) return false;
    if (listener->local_port != dst_port) return false;
    if (listener->local_addr.family != dst_addr->family) return false;
    return tcp_addr_is_unspecified(&listener->local_addr) ||
           tcp_addr_equal(&listener->local_addr, dst_addr);
}

static inline void tcp_write_u16(uint8 *p, uint16 v) {
    p[0] = (uint8)(v >> 8);
    p[1] = (uint8)v;
}

static inline void tcp_write_u32(uint8 *p, uint32 v) {
    p[0] = (uint8)(v >> 24);
    p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);
    p[3] = (uint8)v;
}

static inline uint16 tcp_read_u16(const uint8 *p) {
    return ((uint16)p[0] << 8) | p[1];
}

static inline uint32 tcp_read_u32(const uint8 *p) {
    return ((uint32)p[0] << 24) |
           ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8) |
           (uint32)p[3];
}

void tcp_init(void) {
    memset(connections, 0, sizeof(connections));
}

static uint16 tcp_get_free_port(void) {
    static uint16 next_port = 0;
    
    irq_state_t flags = spinlock_irq_acquire(&tcp_lock);
    
    if (next_port == 0) {
        next_port = TCP_EPHEMERAL_START + (arch_timer_get_ticks() % (TCP_EPHEMERAL_END - TCP_EPHEMERAL_START + 1));
    }
    
    for (int i = 0; i < (TCP_EPHEMERAL_END - TCP_EPHEMERAL_START + 1); i++) {
        uint16 port = next_port;
        if (next_port >= TCP_EPHEMERAL_END) {
            next_port = TCP_EPHEMERAL_START;
        } else {
            next_port++;
        }
        
        //check if port is in use
        bool in_use = false;
        for (int j = 0; j < TCP_MAX_CONNECTIONS; j++) {
            if (connections[j].active && connections[j].local_port == port) {
                in_use = true;
                break;
            }
        }
        
        if (!in_use) {
            spinlock_irq_release(&tcp_lock, flags);
            return port;
        }
    }
    spinlock_irq_release(&tcp_lock, flags);
    return 0;
}

static tcp_conn_t *tcp_alloc_conn_locked(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(tcp_conn_t));
            connections[i].active = true;
            return &connections[i];
        }
    }
    return NULL;
}

static tcp_conn_t *tcp_alloc_conn(void) {
    irq_state_t flags = spinlock_irq_acquire(&tcp_lock);
    tcp_conn_t *conn = tcp_alloc_conn_locked();
    spinlock_irq_release(&tcp_lock, flags);
    return conn;
}

static tcp_conn_t *tcp_find_conn(const net_addr_t *local_addr, uint16 local_port,
                                 const net_addr_t *remote_addr, uint16 remote_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &connections[i];
        if (c->active &&
            tcp_addr_equal(&c->local_addr, local_addr) && c->local_port == local_port &&
            tcp_addr_equal(&c->remote_addr, remote_addr) && c->remote_port == remote_port) {
            return c;
        }
    }
    return NULL;
}

static int tcp_checksum(const net_addr_t *src_addr, const net_addr_t *dst_addr,
                        const void *tcp_data, size tcp_len, uint16 *out_sum) {
    if (src_addr->family == NET_ADDR_FAMILY_IPV6) {
        *out_sum = ipv6_upper_checksum(src_addr->addr.ipv6, dst_addr->addr.ipv6,
                                      IPPROTO_TCP, tcp_data, tcp_len);
        return 0;
    }

    uint32 sum = 0;
    uint8 pseudo[12];
    pseudo[0] = (uint8)(src_addr->addr.ipv4 & 0xFF);
    pseudo[1] = (uint8)((src_addr->addr.ipv4 >> 8) & 0xFF);
    pseudo[2] = (uint8)((src_addr->addr.ipv4 >> 16) & 0xFF);
    pseudo[3] = (uint8)((src_addr->addr.ipv4 >> 24) & 0xFF);
    pseudo[4] = (uint8)(dst_addr->addr.ipv4 & 0xFF);
    pseudo[5] = (uint8)((dst_addr->addr.ipv4 >> 8) & 0xFF);
    pseudo[6] = (uint8)((dst_addr->addr.ipv4 >> 16) & 0xFF);
    pseudo[7] = (uint8)((dst_addr->addr.ipv4 >> 24) & 0xFF);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_TCP;
    pseudo[10] = (uint8)(tcp_len >> 8);
    pseudo[11] = (uint8)tcp_len;

    sum = tcp_checksum_add_be(pseudo, sizeof(pseudo), sum);
    sum = tcp_checksum_add_be((const uint8 *)tcp_data, tcp_len, sum);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    *out_sum = ~sum;
    return 0;
}

static int tcp_send_segment(tcp_conn_t *conn, uint8 flags,
                             const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    size header_len = sizeof(tcp_header_t);
    size total = header_len + payload_len;
    if (total > ETH_MTU) return -1;
    
    irq_state_t lock_flags = spinlock_irq_acquire(&tcp_lock);
    memset(packet, 0, header_len);
    tcp_write_u16(packet + 0, conn->local_port);
    tcp_write_u16(packet + 2, conn->remote_port);
    tcp_write_u32(packet + 4, conn->snd_nxt);
    tcp_write_u32(packet + 8, conn->rcv_nxt);
    
    size opts_len = 0;
    if (flags & TCP_SYN) {
        uint8 *opts = packet + sizeof(tcp_header_t);
        uint16 mss = (conn->remote_addr.family == NET_ADDR_FAMILY_IPV6) ?
                     TCP_MSS_IPV6 : TCP_MSS_IPV4;
        
        //MSS option (kind=2, len=4, value=mss)
        opts[0] = 2;
        opts[1] = 4;
        opts[2] = (uint8)(mss >> 8);
        opts[3] = (uint8)(mss & 0xFF);
        
        //SACK permitted (kind=4, len=2)
        opts[4] = 4;
        opts[5] = 2;
        
        //NOP padding to 4-byte boundary (8 bytes total)
        opts[6] = 1;
        opts[7] = 1;
        
        opts_len = 8;
        header_len += opts_len;
        total += opts_len;
        packet[12] = (uint8)((header_len / 4) << 4);  //7 × 32-bit words = 28 bytes
    } else {
        packet[12] = (5 << 4);  //5 × 32-bit words = 20 bytes
    }
    packet[13] = flags;
    tcp_write_u16(packet + 14, conn->rcv_wnd > 0 ? conn->rcv_wnd : TCP_DEFAULT_WINDOW);
    tcp_write_u16(packet + 16, 0);
    tcp_write_u16(packet + 18, 0);
    
    if (payload_len > 0 && payload) {
        memcpy(packet + header_len, payload, payload_len);
    }

    uint16 checksum = 0;
    int chk_err = tcp_checksum(&conn->local_addr, &conn->remote_addr, packet, total, &checksum);
    if (chk_err != 0) {
        spinlock_irq_release(&tcp_lock, lock_flags);
        return chk_err;
    }
    if (checksum == 0) checksum = 0xFFFF;
    tcp_write_u16(packet + 16, checksum);

    //advance sequence number
    if (flags & TCP_SYN) conn->snd_nxt++;
    if (flags & TCP_FIN) conn->snd_nxt++;
    conn->snd_nxt += payload_len;
    
    netif_t *nif = conn->nif;
    net_addr_t remote_addr = conn->remote_addr;
    spinlock_irq_release(&tcp_lock, lock_flags);
    
    if (remote_addr.family == NET_ADDR_FAMILY_IPV4) {
        return ipv4_send(nif, remote_addr.addr.ipv4, IPPROTO_TCP, packet, total);
    }
    if (remote_addr.family == NET_ADDR_FAMILY_IPV6) {
        return ipv6_send(nif, remote_addr.addr.ipv6, IPPROTO_TCP, packet, total);
    }
    return -1;
}

static void tcp_send_rst(netif_t *nif, const net_addr_t *src_addr,
                         const net_addr_t *dst_addr, uint16 src_port,
                         uint16 dst_port, uint32 seq, uint32 ack) {
    uint8 packet[sizeof(tcp_header_t)];
    memset(packet, 0, sizeof(packet));
    tcp_write_u16(packet + 0, src_port);
    tcp_write_u16(packet + 2, dst_port);
    tcp_write_u32(packet + 4, seq);
    tcp_write_u32(packet + 8, ack);
    packet[12] = (5 << 4);
    packet[13] = TCP_RST | TCP_ACK;
    
    tcp_write_u16(packet + 14, 0);
    tcp_write_u16(packet + 16, 0);
    tcp_write_u16(packet + 18, 0);
    uint16 checksum = 0;
    int chk_err = tcp_checksum(src_addr, dst_addr, packet, sizeof(tcp_header_t), &checksum);
    if (chk_err != 0) {
        return;
    }
    if (checksum == 0) checksum = 0xFFFF;
    tcp_write_u16(packet + 16, checksum);

    if (dst_addr->family == NET_ADDR_FAMILY_IPV4) {
        ipv4_send(nif, dst_addr->addr.ipv4, IPPROTO_TCP, packet, sizeof(tcp_header_t));
    } else if (dst_addr->family == NET_ADDR_FAMILY_IPV6) {
        ipv6_send(nif, dst_addr->addr.ipv6, IPPROTO_TCP, packet, sizeof(tcp_header_t));
    }
}

static void tcp_recv_common(netif_t *nif, const net_addr_t *src_addr,
                            const net_addr_t *dst_addr, void *data, size len) {
    if (len < sizeof(tcp_header_t)) return;

    //verify checksum
    uint16 sum;
    if (tcp_checksum(src_addr, dst_addr, data, len, &sum) != 0) return;
    if (sum != 0) {
        printf("[tcp] Dropped packet: bad checksum 0x%04x\n", sum);
        return;
    }
    
    const uint8 *tcp = (const uint8 *)data;
    uint16 src_port = tcp_read_u16(tcp + 0);
    uint16 dst_port = tcp_read_u16(tcp + 2);
    uint32 seq = tcp_read_u32(tcp + 4);
    uint32 ack = tcp_read_u32(tcp + 8);
    uint8 data_off_raw = tcp[12] >> 4;
    uint8 flags = tcp[13];
    uint8 data_off = data_off_raw * 4;
    
    if (data_off < sizeof(tcp_header_t) || data_off > len) {
        return;
    }
    
    void *payload = (uint8 *)data + data_off;
    size payload_len = len - data_off;

    irq_state_t lock_flags = spinlock_irq_acquire(&tcp_lock);
    tcp_conn_t *conn = tcp_find_conn(dst_addr, dst_port, src_addr, src_port);
    
    if (!conn) {
        //check for listening sockets on this port
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
            tcp_conn_t *l = &connections[i];
            if (tcp_listener_matches(l, dst_addr, dst_port)) {
                //incoming SYN on a listening socket - create new connection
                if (flags & TCP_SYN) {
                    tcp_conn_t *newconn = tcp_alloc_conn_locked();
                    if (!newconn) {
                        spinlock_irq_release(&tcp_lock, lock_flags);
                        return;
                    }
                    
                    newconn->nif = nif;
                    newconn->local_addr = *dst_addr;
                    newconn->local_port = dst_port;
                    newconn->remote_addr = *src_addr;
                    newconn->remote_port = src_port;
                    newconn->state = TCP_STATE_SYN_RECEIVED;
                    newconn->rcv_nxt = seq + 1;
                    newconn->snd_nxt = (uint32)arch_timer_get_ticks();
                    newconn->snd_una = newconn->snd_nxt;
                    newconn->rcv_wnd = TCP_DEFAULT_WINDOW;
                    //release lock before sending
                    spinlock_irq_release(&tcp_lock, lock_flags);

                    //send SYN-ACK
                    tcp_send_segment(newconn, TCP_SYN | TCP_ACK, NULL, 0);
                } else {
                    spinlock_irq_release(&tcp_lock, lock_flags);
                }
                return;
            }
        }
        
        spinlock_irq_release(&tcp_lock, lock_flags);

        //no connection and no listener send RST
        if (!(flags & TCP_RST)) {
            if (flags & TCP_ACK) {
                tcp_send_rst(nif, dst_addr, src_addr, dst_port, src_port, ack, 0);
            } else {
                uint32 rst_ack = seq + payload_len;
                if (flags & TCP_SYN) rst_ack++;
                if (flags & TCP_FIN) rst_ack++;
                tcp_send_rst(nif, dst_addr, src_addr, dst_port, src_port, 0, rst_ack);
            }
        }
        return;
    }
    
    switch (conn->state) {
        case TCP_STATE_SYN_RECEIVED:
            if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
                conn->snd_una = ack;
                conn->state = TCP_STATE_ESTABLISHED;
                spinlock_irq_release(&tcp_lock, lock_flags);
                return;
            }
            break;
            
        case TCP_STATE_SYN_SENT:
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                if (ack == conn->snd_nxt) {
                    conn->rcv_nxt = seq + 1;
                    conn->snd_una = ack;
                    conn->state = TCP_STATE_ESTABLISHED;
                    spinlock_irq_release(&tcp_lock, lock_flags);
                    //send ACK
                    tcp_send_segment(conn, TCP_ACK, NULL, 0);
                    return;
                }
            }
            break;
            
        case TCP_STATE_ESTABLISHED:
            if (flags & TCP_RST) {
                conn->state = TCP_STATE_CLOSED;
                conn->active = false;
                spinlock_irq_release(&tcp_lock, lock_flags);
                return;
            }

            //handle ACK
            if (flags & TCP_ACK) {
                conn->snd_una = ack;
            }
            
            //handle incoming data
            if (payload_len > 0 && seq == conn->rcv_nxt) {
                size space = TCP_RX_BUF_SIZE - conn->rx_len;
                size copy = (payload_len < space) ? payload_len : space;
                if (copy > 0) {
                    memcpy(conn->rx_buf + conn->rx_len, payload, copy);
                    conn->rx_len += copy;
                    conn->rcv_nxt += copy; //only advance by what was actually buffered
                    
                    spinlock_irq_release(&tcp_lock, lock_flags);
                    //send ACK for the newly buffered data
                    tcp_send_segment(conn, TCP_ACK, NULL, 0);
                    return;
                } else {
                    spinlock_irq_release(&tcp_lock, lock_flags);
                    //buffer full, don't advance rcv_nxt and don't ACK
                    return;
                }
            }
            
            //handle FIN
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + payload_len + 1;
                conn->state = TCP_STATE_CLOSE_WAIT;
                spinlock_irq_release(&tcp_lock, lock_flags);
                tcp_send_segment(conn, TCP_ACK, NULL, 0);
                return;
            }
            break;
            
        case TCP_STATE_FIN_WAIT_1:
            if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
                if (flags & TCP_FIN) {
                    conn->rcv_nxt = seq + 1;
                    conn->state = TCP_STATE_CLOSED;
                    conn->active = false;
                    spinlock_irq_release(&tcp_lock, lock_flags);
                    tcp_send_segment(conn, TCP_ACK, NULL, 0);
                    return;
                } else {
                    conn->state = TCP_STATE_FIN_WAIT_2;
                }
            }
            break;
            
        case TCP_STATE_FIN_WAIT_2:
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + 1;
                conn->state = TCP_STATE_CLOSED;
                conn->active = false;
                spinlock_irq_release(&tcp_lock, lock_flags);
                tcp_send_segment(conn, TCP_ACK, NULL, 0);
                return;
            }
            break;
            
        case TCP_STATE_LAST_ACK:
            if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
                conn->state = TCP_STATE_CLOSED;
                conn->active = false;
            }
            break;

        default:
            break;
    }
    spinlock_irq_release(&tcp_lock, lock_flags);
}

void tcp_recv(netif_t *nif, uint32 src_ip, uint32 dst_ip, void *data, size len) {
    net_addr_t src_addr;
    net_addr_t dst_addr;
    tcp_addr_from_ipv4(&src_addr, src_ip);
    tcp_addr_from_ipv4(&dst_addr, dst_ip);
    tcp_recv_common(nif, &src_addr, &dst_addr, data, len);
}

void tcp_recv_ipv6(netif_t *nif, const uint8 src_ip[NET_IPV6_ADDR_LEN],
                   const uint8 dst_ip[NET_IPV6_ADDR_LEN], void *data, size len) {
    net_addr_t src_addr;
    net_addr_t dst_addr;
    tcp_addr_from_ipv6(&src_addr, src_ip);
    tcp_addr_from_ipv6(&dst_addr, dst_ip);
    tcp_recv_common(nif, &src_addr, &dst_addr, data, len);
}

tcp_conn_t *tcp_connect_addr(netif_t *nif, const net_addr_t *dst_addr,
                             uint16 dst_port, uint16 src_port) {
    if (src_port == 0) {
        src_port = tcp_get_free_port();
        if (src_port == 0) return NULL;
    }
    
    irq_state_t lock_flags = spinlock_irq_acquire(&tcp_lock);
    tcp_conn_t *conn = tcp_alloc_conn_locked();
    if (!conn) {
        spinlock_irq_release(&tcp_lock, lock_flags);
        return NULL;
    }
    
    conn->nif = nif;
    if (dst_addr->family == NET_ADDR_FAMILY_IPV4) {
        if (nif->ip_addr == 0) {
            conn->active = false;
            spinlock_irq_release(&tcp_lock, lock_flags);
            return NULL;
        }
        tcp_addr_from_ipv4(&conn->local_addr, nif->ip_addr);
    } else if (dst_addr->family == NET_ADDR_FAMILY_IPV6) {
        if (ipv6_addr_is_unspecified(nif->ipv6_addr)) {
            conn->active = false;
            spinlock_irq_release(&tcp_lock, lock_flags);
            return NULL;
        }
        tcp_addr_from_ipv6(&conn->local_addr, nif->ipv6_addr);
    } else {
        conn->active = false;
        spinlock_irq_release(&tcp_lock, lock_flags);
        return NULL;
    }
    conn->local_port = src_port;
    conn->remote_addr = *dst_addr;
    conn->remote_port = dst_port;
    conn->state = TCP_STATE_SYN_SENT;
    conn->snd_nxt = (uint32)(arch_timer_get_ticks() & 0xFFFFFFFF);
    conn->snd_una = conn->snd_nxt;
    conn->rcv_wnd = TCP_DEFAULT_WINDOW;
    conn->rx_len = 0;
    spinlock_irq_release(&tcp_lock, lock_flags);
    
    //send SYN
    tcp_send_segment(conn, TCP_SYN, NULL, 0);
    
    //wait for SYN-ACK with retransmission (3 attempts 2 sec each)
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;
    
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64 start = arch_timer_get_ticks();
        uint64 timeout = (uint64)freq * 2;
        
        while (arch_timer_get_ticks() - start < timeout) {
            if (proc_current_should_abort_blocking()) {
                //Do not send RST on local async abort; the peer will retransmit until timeout.
                lock_flags = spinlock_irq_acquire(&tcp_lock);
                conn->state = TCP_STATE_CLOSED;
                conn->active = false;
                spinlock_irq_release(&tcp_lock, lock_flags);
                return NULL;
            }
            if (conn->state == TCP_STATE_ESTABLISHED) return conn;
            net_poll();
            sched_yield();
        }
        
        //retransmit SYN
        if (attempt < 2) {
            lock_flags = spinlock_irq_acquire(&tcp_lock);
            conn->snd_nxt = conn->snd_una; //reset seq for retransmit
            spinlock_irq_release(&tcp_lock, lock_flags);
            tcp_send_segment(conn, TCP_SYN, NULL, 0);
        }
    }
    
    lock_flags = spinlock_irq_acquire(&tcp_lock);
    conn->state = TCP_STATE_CLOSED;
    conn->active = false;
    spinlock_irq_release(&tcp_lock, lock_flags);
    return NULL;
}

int tcp_send(tcp_conn_t *conn, const void *data, size len) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) return -1;
    
    const uint8 *ptr = (const uint8 *)data;
    size remaining = len;
    size mss = (conn->remote_addr.family == NET_ADDR_FAMILY_IPV6) ?
               TCP_MSS_IPV6 : TCP_MSS_IPV4;
    
    while (remaining > 0) {
        size chunk = (remaining < mss) ? remaining : mss;
        int res = tcp_send_segment(conn, TCP_ACK | TCP_PSH, ptr, chunk);
        if (res != 0) return -1;
        ptr += chunk;
        remaining -= chunk;
    }
    
    return 0;
}

int tcp_read(tcp_conn_t *conn, void *buf, size len) {
    if (!conn) return -1;
    
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;
    uint64 start = arch_timer_get_ticks();
    uint64 timeout = (uint64)freq * 10; //10 seconds
    
    irq_state_t flags = spinlock_irq_acquire(&tcp_lock);
    while (conn->rx_len == 0) {
        if (proc_current_should_abort_blocking()) {
            spinlock_irq_release(&tcp_lock, flags);
            return -1;
        }
        if (conn->state != TCP_STATE_ESTABLISHED && 
            conn->state != TCP_STATE_SYN_SENT &&
            conn->state != TCP_STATE_SYN_RECEIVED &&
            conn->state != TCP_STATE_CLOSE_WAIT &&
            conn->state != TCP_STATE_FIN_WAIT_1 &&
            conn->state != TCP_STATE_FIN_WAIT_2) {
            spinlock_irq_release(&tcp_lock, flags);
            return 0; //connection closed/closing and no data
        }
        
        if (arch_timer_get_ticks() - start > timeout) {
            spinlock_irq_release(&tcp_lock, flags);
            return -1; //timeout
        }
        
        spinlock_irq_release(&tcp_lock, flags);
        net_poll();
        sched_yield();
        flags = spinlock_irq_acquire(&tcp_lock);
    }
    
    size copy = (conn->rx_len < len) ? conn->rx_len : len;
    memcpy(buf, conn->rx_buf, copy);
    
    //shift remaining data
    if (copy < conn->rx_len) {
        memmove(conn->rx_buf, conn->rx_buf + copy, conn->rx_len - copy);
    }
    conn->rx_len -= copy;
    
    spinlock_irq_release(&tcp_lock, flags);
    return (int)copy;
}

int tcp_close(tcp_conn_t *conn) {
    if (!conn) return -1;
    
    if (conn->state == TCP_STATE_ESTABLISHED) {
        conn->state = TCP_STATE_FIN_WAIT_1;
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, NULL, 0);
    } else if (conn->state == TCP_STATE_CLOSE_WAIT) {
        conn->state = TCP_STATE_LAST_ACK;
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, NULL, 0);
    }
    
    return 0;
}

tcp_conn_t *tcp_listen_addr(netif_t *nif, const net_addr_t *local_addr, uint16 port) {
    tcp_conn_t *conn = tcp_alloc_conn();
    if (!conn) return NULL;
    
    conn->nif = nif;
    conn->local_addr = *local_addr;
    conn->local_port = port;
    conn->state = TCP_STATE_LISTEN;
    conn->listening = true;
    
    return conn;
}

tcp_conn_t *tcp_accept(tcp_conn_t *listener) {
    if (!listener || !listener->listening) return NULL;
    
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;
    uint64 timeout = (uint64)freq * 30; //30 second accept timeout
    uint64 start = arch_timer_get_ticks();
    
    while (arch_timer_get_ticks() - start < timeout) {
        if (proc_current_should_abort_blocking()) {
            return NULL;
        }

        //scan for connections on this port that completed handshake
        irq_state_t scan_flags = spinlock_irq_acquire(&tcp_lock);
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
            tcp_conn_t *c = &connections[i];
            if (c->active && !c->listening && !c->accepted &&
                c->local_port == listener->local_port &&
                (tcp_addr_is_unspecified(&listener->local_addr) ||
                 tcp_addr_equal(&c->local_addr, &listener->local_addr)) &&
                c->state == TCP_STATE_ESTABLISHED) {
                c->accepted = true;
                spinlock_irq_release(&tcp_lock, scan_flags);
                return c;   
            }
        }
        spinlock_irq_release(&tcp_lock, scan_flags);
        net_poll();
        sched_yield();
    }
    
    return NULL; //timeout
}
