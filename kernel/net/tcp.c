#include <net/tcp.h>
#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <arch/cpu.h>
#include <arch/timer.h>

static tcp_conn_t connections[TCP_MAX_CONNECTIONS];
static spinlock_irq_t tcp_lock = {SPINLOCK_INIT, 0};

void tcp_init(void) {
    memset(connections, 0, sizeof(connections));
}

static uint16 tcp_get_free_port(void) {
    static uint16 next_port = TCP_EPHEMERAL_START;
    
    spinlock_irq_acquire(&tcp_lock);
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
            spinlock_irq_release(&tcp_lock);
            return port;
        }
    }
    spinlock_irq_release(&tcp_lock);
    return 0;
}

static tcp_conn_t *tcp_alloc_conn(void) {
    spinlock_irq_acquire(&tcp_lock);
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(tcp_conn_t));
            connections[i].active = true;
            spinlock_irq_release(&tcp_lock);
            return &connections[i];
        }
    }
    spinlock_irq_release(&tcp_lock);
    return NULL;
}

static tcp_conn_t *tcp_find_conn(uint32 local_ip, uint16 local_port,
                                  uint32 remote_ip, uint16 remote_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = &connections[i];
        if (c->active &&
            c->local_ip == local_ip && c->local_port == local_port &&
            c->remote_ip == remote_ip && c->remote_port == remote_port) {
            return c;
        }
    }
    return NULL;
}

static uint16 tcp_checksum(uint32 src_ip, uint32 dst_ip,
                            const void *tcp_data, size tcp_len) {
    uint32 sum = 0;
    
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
    
    tcp->checksum = tcp_checksum(conn->local_ip, conn->remote_ip, packet, total);
    
    //advance sequence number
    if (flags & TCP_SYN) conn->snd_nxt++;
    if (flags & TCP_FIN) conn->snd_nxt++;
    conn->snd_nxt += payload_len;
    
    printf("[tcp] TX flags=0x%02x to ", flags);
    net_print_ip(conn->remote_ip);
    printf(":%u\n", conn->remote_port);
    
    return ipv4_send(conn->nif, conn->remote_ip, IPPROTO_TCP, packet, total);
}

static void tcp_send_rst(netif_t *nif, uint32 dst_ip,
                          uint16 src_port, uint16 dst_port,
                          uint32 seq, uint32 ack) {
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
    
    tcp->checksum = tcp_checksum(nif->ip_addr, dst_ip,
                                 packet, sizeof(tcp_header_t));
    
    ipv4_send(nif, dst_ip, IPPROTO_TCP, packet, sizeof(tcp_header_t));
}

void tcp_recv(netif_t *nif, uint32 src_ip, uint32 dst_ip, void *data, size len) {
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
    net_print_ip(src_ip);
    printf(":%u -> :%u seq=%u ack=%u\n", src_port, dst_port, seq, ack);
    
    tcp_conn_t *conn = tcp_find_conn(dst_ip, dst_port, src_ip, src_port);
    
    if (!conn) {
        //check for listening sockets on this port
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
            tcp_conn_t *l = &connections[i];
            if (l->active && l->listening && l->local_port == dst_port) {
                //incoming SYN on a listening socket - create new connection
                if (flags & TCP_SYN) {
                    tcp_conn_t *newconn = tcp_alloc_conn();
                    if (!newconn) return;
                    
                    newconn->nif = nif;
                    newconn->local_ip = dst_ip;
                    newconn->local_port = dst_port;
                    newconn->remote_ip = src_ip;
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
                tcp_send_rst(nif, src_ip, dst_port, src_port, ack, 0);
            } else {
                uint32 rst_ack = seq + payload_len;
                if (flags & TCP_SYN) rst_ack++;
                if (flags & TCP_FIN) rst_ack++;
                tcp_send_rst(nif, src_ip, dst_port, src_port, 0, rst_ack);
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
                net_print_ip(conn->remote_ip);
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
                    net_print_ip(conn->remote_ip);
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

tcp_conn_t *tcp_connect(netif_t *nif, uint32 dst_ip, uint16 dst_port, uint16 src_port) {
    if (src_port == 0) {
        src_port = tcp_get_free_port();
        if (src_port == 0) return NULL;
    }
    
    tcp_conn_t *conn = tcp_alloc_conn();
    if (!conn) return NULL;
    
    conn->nif = nif;
    conn->local_ip = nif->ip_addr;
    conn->local_port = src_port;
    conn->remote_ip = dst_ip;
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

int tcp_send(tcp_conn_t *conn, const void *data, size len) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) return -1;
    
    const uint8 *ptr = (const uint8 *)data;
    size remaining = len;
    
    while (remaining > 0) {
        size chunk = (remaining < TCP_MSS) ? remaining : TCP_MSS;
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

tcp_conn_t *tcp_listen(netif_t *nif, uint16 port) {
    tcp_conn_t *conn = tcp_alloc_conn();
    if (!conn) return NULL;
    
    conn->nif = nif;
    conn->local_ip = nif->ip_addr;
    conn->local_port = port;
    conn->state = TCP_STATE_LISTEN;
    conn->listening = true;
    
    printf("[tcp] Listening on port %u\n", port);
    return conn;
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
                c->state == TCP_STATE_ESTABLISHED) {
                c->accepted = true;
                return c;
            }
        }
        arch_pause();
    }
    
    return NULL; //timeout
}
