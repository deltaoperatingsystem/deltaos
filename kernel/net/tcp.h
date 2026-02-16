#ifndef NET_TCP_H
#define NET_TCP_H

#include <arch/types.h>
#include <net/net.h>
#include <arch/timer.h>

//TCP flags
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

//TCP connection states
typedef enum {
    TCP_STATE_CLOSED,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSING,
    TCP_STATE_TIME_WAIT,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK
} tcp_state_t;

typedef struct __attribute__((packed)) {
    uint16 src_port;    //big-endian
    uint16 dst_port;    //big-endian
    uint32 seq_num;     //big-endian
    uint32 ack_num;     //big-endian
    uint8  data_off;    //data offset in upper 4 bits (in 32-bit words)
    uint8  flags;       //TCP flags
    uint16 window;      //big-endian
    uint16 checksum;    //big-endian
    uint16 urgent;      //big-endian
} tcp_header_t;

//TCP pseudo-header for checksum
typedef struct __attribute__((packed)) {
    uint32 src_ip;
    uint32 dst_ip;
    uint8  zero;
    uint8  protocol;
    uint16 tcp_len;
} tcp_pseudo_header_t;

#define TCP_MAX_CONNECTIONS 16
#define TCP_RX_BUF_SIZE     4096
#define TCP_TX_BUF_SIZE     4096
#define TCP_MSS             1460  //max segment size (ETH_MTU - IP header - TCP header)
#define TCP_DEFAULT_WINDOW  4096
#define TCP_EPHEMERAL_START 49152
#define TCP_EPHEMERAL_END   65535

//TCP connection block
typedef struct tcp_conn {
    tcp_state_t state;
    
    //endpoints
    uint32 local_ip;
    uint16 local_port;
    uint32 remote_ip;
    uint16 remote_port;
    
    //sequence numbers
    uint32 snd_una;    //send unacknowledged
    uint32 snd_nxt;    //send next
    uint32 rcv_nxt;    //receive next expected
    uint16 rcv_wnd;    //receive window
    
    //receive buffer
    uint8  rx_buf[TCP_RX_BUF_SIZE];
    volatile size rx_len;
    
    //network interface
    netif_t *nif;
    
    //retransmission
    uint64 retransmit_at;   //tick at which to retransmit
    uint8  retransmit_count;
    
    bool active;
    bool listening;         //true if this is a listening socket
    bool accepted;          //true if this connection has been returned by tcp_accept
} tcp_conn_t;

//receive a TCP segment (called from IPv4 layer)
void tcp_recv(netif_t *nif, uint32 src_ip, uint32 dst_ip, void *data, size len);

//create a TCP connection (active open)
tcp_conn_t *tcp_connect(netif_t *nif, uint32 dst_ip, uint16 dst_port, uint16 src_port);

//send data on an established connection
int tcp_send(tcp_conn_t *conn, const void *data, size len);

//read received data from a connection
int tcp_read(tcp_conn_t *conn, void *buf, size len);

//close a connection (graceful)
int tcp_close(tcp_conn_t *conn);

//passive open: listen on a port
tcp_conn_t *tcp_listen(netif_t *nif, uint16 port);

//accept an incoming connection on a listening socket
tcp_conn_t *tcp_accept(tcp_conn_t *listener);

//initialize TCP subsystem
void tcp_init(void);

#endif
