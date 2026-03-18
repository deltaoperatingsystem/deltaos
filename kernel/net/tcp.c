#include <net/tcp.h>
#include <net/ipv4.h>
#include <net/ipv6.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <arch/cpu.h>
#include <arch/timer.h>

static tcp_conn_t connections[TCP_MAX_CONNECTIONS];
static spinlock_irq_t tcp_lock = SPINLOCK_IRQ_INIT;

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

void tcp_init(void) {
    memset(connections, 0, sizeof(connections));
}

static uint16 tcp_get_free_port(void) {
    static uint16 next_port = TCP_EPHEMERAL_START;
    
    irq_state_t flags = spinlock_irq_acquire(&tcp_lock);
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

static tcp_conn_t *tcp_alloc_conn(void) {
    irq_state_t flags = spinlock_irq_acquire(&tcp_lock);
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(tcp_conn_t));
            connections[i].active = true;
            spinlock_irq_release(&tcp_lock, flags);
            return &connections[i];
        }
    }
    spinlock_irq_release(&tcp_lock, flags);
    return NULL;
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

static uint16 tcp_checksum(const net_addr_t *src_addr, const net_addr_t *dst_addr,
                           const void *tcp_data, size tcp_len) {
    if (src_addr->family == NET_ADDR_FAMILY_IPV6) {
        return ipv6_upper_checksum(src_addr->addr.ipv6, dst_addr->addr.ipv6,
                                   IPPROTO_TCP, tcp_data, tcp_len);
    }

    uint32 sum = 0;
    uint32 src_ip = src_addr->addr.ipv4;
    uint32 dst_ip = dst_addr->addr.ipv4;
    
    //sum pseudo-header
    //IPs are in network byte order; split each into two uint16 halves
    sum += src_ip & 0xFFFF;
    sum += src_ip >> 16;
    sum += dst_ip & 0xFFFF;
    sum += dst_ip >> 16;
    sum += htons(IPPROTO_TCP);
    sum += htons((uint16)tcp_len);
    
    //sum TCP segment data
    const uint16 *ptr = (const uint16 *)tcp_data;
    size remaining = tcp_len;
    while (remaining > 1) {
        sum += *ptr++;
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += *(const uint8 *)ptr;
    }
    
    //fold 32-bit sum into 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16)~sum;
}

static int tcp_send_segment(tcp_conn_t *conn, uint8 flags,
                             const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    size header_len = sizeof(tcp_header_t);
    size total = header_len + payload_len;
    if (total > ETH_MTU) return -1;
    
    tcp_header_t *tcp = (tcp_header_t *)packet;
    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq_num = htonl(conn->snd_nxt);
    tcp->ack_num = htonl(conn->rcv_nxt);
    tcp->data_off = (5 << 4);  //5 × 32-bit words = 20 bytes
    tcp->flags = flags;
    tcp->window = htons(conn->rcv_wnd > 0 ? conn->rcv_wnd : TCP_DEFAULT_WINDOW);
    tcp->checksum = 0;
    tcp->urgent = 0;
    
    if (payload_len > 0 && payload) {
        memcpy(packet + header_len, payload, payload_len);
    }
    
    tcp->checksum = tcp_checksum(&conn->local_addr, &conn->remote_addr, packet, total);
    
    //advance sequence number
    if (flags & TCP_SYN) conn->snd_nxt++;
    if (flags & TCP_FIN) conn->snd_nxt++;
    conn->snd_nxt += payload_len;
    
    printf("[tcp] TX flags=0x%02x to ", flags);
    net_print_addr(&conn->remote_addr);
    printf(":%u\n", conn->remote_port);
    
    if (conn->remote_addr.family == NET_ADDR_FAMILY_IPV4) {
        return ipv4_send(conn->nif, conn->remote_addr.addr.ipv4, IPPROTO_TCP, packet, total);
    }
    if (conn->remote_addr.family == NET_ADDR_FAMILY_IPV6) {
        return ipv6_send(conn->nif, conn->remote_addr.addr.ipv6, IPPROTO_TCP, packet, total);
    }
    return -1;
}

static void tcp_send_rst(netif_t *nif, const net_addr_t *src_addr,
                         const net_addr_t *dst_addr, uint16 src_port,
                         uint16 dst_port, uint32 seq, uint32 ack) {
    uint8 packet[sizeof(tcp_header_t)];
    tcp_header_t *tcp = (tcp_header_t *)packet;
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq_num = htonl(seq);
    tcp->ack_num = htonl(ack);
    tcp->data_off = (5 << 4);
    tcp->flags = TCP_RST | TCP_ACK;
    tcp->window = 0;
    tcp->checksum = 0;
    tcp->urgent = 0;
    
    tcp->checksum = tcp_checksum(src_addr, dst_addr, packet, sizeof(tcp_header_t));
    
    if (dst_addr->family == NET_ADDR_FAMILY_IPV4) {
        ipv4_send(nif, dst_addr->addr.ipv4, IPPROTO_TCP, packet, sizeof(tcp_header_t));
    } else if (dst_addr->family == NET_ADDR_FAMILY_IPV6) {
        ipv6_send(nif, dst_addr->addr.ipv6, IPPROTO_TCP, packet, sizeof(tcp_header_t));
    }
}

static void tcp_recv_common(netif_t *nif, const net_addr_t *src_addr,
                            const net_addr_t *dst_addr, void *data, size len) {
    if (len < sizeof(tcp_header_t)) return;
    
    tcp_header_t *tcp = (tcp_header_t *)data;
    uint16 src_port = ntohs(tcp->src_port);
    uint16 dst_port = ntohs(tcp->dst_port);
    uint32 seq = ntohl(tcp->seq_num);
    uint32 ack = ntohl(tcp->ack_num);
    uint8 flags = tcp->flags;
    uint8 data_off_raw = tcp->data_off >> 4;
    uint8 data_off = data_off_raw * 4;
    
    if (data_off < sizeof(tcp_header_t) || data_off > len) {
        printf("[tcp] Dropping invalid TCP packet: data_off=%u, len=%u\n", data_off, (uint32)len);
        return;
    }
    
    void *payload = (uint8 *)data + data_off;
    size payload_len = len - data_off;
    
    printf("[tcp] RX flags=0x%02x from ", flags);
    net_print_addr(src_addr);
    printf(":%u -> :%u seq=%u ack=%u\n", src_port, dst_port, seq, ack);
    
    tcp_conn_t *conn = tcp_find_conn(dst_addr, dst_port, src_addr, src_port);
    
    if (!conn) {
        //check for listening sockets on this port
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
            tcp_conn_t *l = &connections[i];
            if (tcp_listener_matches(l, dst_addr, dst_port)) {
                //incoming SYN on a listening socket - create new connection
                if (flags & TCP_SYN) {
                    tcp_conn_t *newconn = tcp_alloc_conn();
                    if (!newconn) return;
                    
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
                    
                    //send SYN-ACK
                    tcp_send_segment(newconn, TCP_SYN | TCP_ACK, NULL, 0);
                }
                return;
            }
        }
        
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
                printf("[tcp] Connection established from ");
                net_print_addr(&conn->remote_addr);
                printf(":%u\n", conn->remote_port);
            }
            break;
            
        case TCP_STATE_SYN_SENT:
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                if (ack == conn->snd_nxt) {
                    conn->rcv_nxt = seq + 1;
                    conn->snd_una = ack;
                    conn->state = TCP_STATE_ESTABLISHED;
                    
                    //send ACK
                    tcp_send_segment(conn, TCP_ACK, NULL, 0);
                    printf("[tcp] Connection established to ");
                    net_print_addr(&conn->remote_addr);
                    printf(":%u\n", conn->remote_port);
                }
            }
            break;
            
        case TCP_STATE_ESTABLISHED:
            if (flags & TCP_RST) {
                conn->state = TCP_STATE_CLOSED;
                conn->active = false;
                printf("[tcp] Connection reset by peer\n");
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
                    
                    //send ACK for the newly buffered data
                    tcp_send_segment(conn, TCP_ACK, NULL, 0);
                } else {
                    //buffer full, don't advance rcv_nxt and don't ACK
                    printf("[tcp] Receive buffer full, dropping payload\n");
                }
            }
            
            //handle FIN
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + payload_len + 1;
                conn->state = TCP_STATE_CLOSE_WAIT;
                tcp_send_segment(conn, TCP_ACK, NULL, 0);
                printf("[tcp] Peer sent FIN, connection closing\n");
            }
            break;
            
        case TCP_STATE_FIN_WAIT_1:
            if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
                if (flags & TCP_FIN) {
                    conn->rcv_nxt = seq + 1;
                    conn->state = TCP_STATE_CLOSED;
                    tcp_send_segment(conn, TCP_ACK, NULL, 0);
                    conn->active = false;
                } else {
                    conn->state = TCP_STATE_FIN_WAIT_2;
                }
            }
            break;
            
        case TCP_STATE_FIN_WAIT_2:
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + 1;
                conn->state = TCP_STATE_CLOSED;
                tcp_send_segment(conn, TCP_ACK, NULL, 0);
                conn->active = false;
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

static tcp_conn_t *tcp_connect_addr(netif_t *nif, const net_addr_t *dst_addr,
                                    uint16 dst_port, uint16 src_port) {
    if (src_port == 0) {
        src_port = tcp_get_free_port();
        if (src_port == 0) return NULL;
    }
    
    tcp_conn_t *conn = tcp_alloc_conn();
    if (!conn) return NULL;
    
    conn->nif = nif;
    if (dst_addr->family == NET_ADDR_FAMILY_IPV4) {
        if (nif->ip_addr == 0) {
            conn->active = false;
            return NULL;
        }
        tcp_addr_from_ipv4(&conn->local_addr, nif->ip_addr);
    } else if (dst_addr->family == NET_ADDR_FAMILY_IPV6) {
        if (ipv6_addr_is_unspecified(nif->ipv6_addr)) {
            conn->active = false;
            return NULL;
        }
        tcp_addr_from_ipv6(&conn->local_addr, nif->ipv6_addr);
    } else {
        conn->active = false;
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
    
    //send SYN
    tcp_send_segment(conn, TCP_SYN, NULL, 0);
    
    //wait for SYN-ACK with retransmission (3 attempts 2 sec each)
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;
    
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64 start = arch_timer_get_ticks();
        uint64 timeout = (uint64)freq * 2;
        
        while (arch_timer_get_ticks() - start < timeout) {
            if (conn->state == TCP_STATE_ESTABLISHED) return conn;
            arch_pause();
        }
        
        //retransmit SYN
        if (attempt < 2) {
            conn->snd_nxt = conn->snd_una; //reset seq for retransmit
            tcp_send_segment(conn, TCP_SYN, NULL, 0);
        }
    }
    
    printf("[tcp] Connection timed out\n");
    conn->state = TCP_STATE_CLOSED;
    conn->active = false;
    return NULL;
}

tcp_conn_t *tcp_connect(netif_t *nif, uint32 dst_ip, uint16 dst_port, uint16 src_port) {
    net_addr_t dst_addr;
    tcp_addr_from_ipv4(&dst_addr, dst_ip);
    return tcp_connect_addr(nif, &dst_addr, dst_port, src_port);
}

tcp_conn_t *tcp_connect_ipv6(netif_t *nif, const uint8 dst_ip[NET_IPV6_ADDR_LEN],
                             uint16 dst_port, uint16 src_port) {
    net_addr_t dst_addr;
    tcp_addr_from_ipv6(&dst_addr, dst_ip);
    return tcp_connect_addr(nif, &dst_addr, dst_port, src_port);
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
    
    size copy = (conn->rx_len < len) ? conn->rx_len : len;
    if (copy == 0) return 0;
    
    memcpy(buf, conn->rx_buf, copy);
    
    //shift remaining data
    if (copy < conn->rx_len) {
        memmove(conn->rx_buf, conn->rx_buf + copy, conn->rx_len - copy);
    }
    conn->rx_len -= copy;
    
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

static tcp_conn_t *tcp_listen_addr(netif_t *nif, const net_addr_t *local_addr, uint16 port) {
    tcp_conn_t *conn = tcp_alloc_conn();
    if (!conn) return NULL;
    
    conn->nif = nif;
    conn->local_addr = *local_addr;
    conn->local_port = port;
    conn->state = TCP_STATE_LISTEN;
    conn->listening = true;
    
    printf("[tcp] Listening on port %u\n", port);
    return conn;
}

tcp_conn_t *tcp_listen(netif_t *nif, uint16 port) {
    net_addr_t local_addr;
    tcp_addr_from_ipv4(&local_addr, 0);
    return tcp_listen_addr(nif, &local_addr, port);
}

tcp_conn_t *tcp_listen_ipv6(netif_t *nif, uint16 port) {
    net_addr_t local_addr;
    local_addr.family = NET_ADDR_FAMILY_IPV6;
    memset(local_addr.addr.ipv6, 0, NET_IPV6_ADDR_LEN);
    return tcp_listen_addr(nif, &local_addr, port);
}

tcp_conn_t *tcp_accept(tcp_conn_t *listener) {
    if (!listener || !listener->listening) return NULL;
    
    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;
    uint64 timeout = (uint64)freq * 30; //30 second accept timeout
    uint64 start = arch_timer_get_ticks();
    
    while (arch_timer_get_ticks() - start < timeout) {
        //scan for connections on this port that completed handshake
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
            tcp_conn_t *c = &connections[i];
            if (c->active && !c->listening && !c->accepted &&
                c->local_port == listener->local_port &&
                (tcp_addr_is_unspecified(&listener->local_addr) ||
                 tcp_addr_equal(&c->local_addr, &listener->local_addr)) &&
                c->state == TCP_STATE_ESTABLISHED) {
                c->accepted = true;
                return c;
            }
        }
        arch_pause();
    }
    
    return NULL; //timeout
}
