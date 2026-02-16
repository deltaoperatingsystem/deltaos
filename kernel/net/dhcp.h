#ifndef NET_DHCP_H
#define NET_DHCP_H

#include <arch/types.h>
#include <net/net.h>

/*
 *DHCP client (RFC 2131)
 *
 *DHCP uses BOOTP message format over UDP:
 * - slient -> erver: port 68 -> 67
 * - server -> client: port 67 -> 68
 */

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

//BOOTP opcodes
#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2

//hardware type
#define DHCP_HTYPE_ETH   1

//DHCP magic cookie (in network byte order as stored)
#define DHCP_MAGIC_COOKIE 0x63538263  //99.130.83.99

//DHCP message types (option 53)
#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_DECLINE   4
#define DHCP_ACK       5
#define DHCP_NAK       6
#define DHCP_RELEASE   7

//DHCP options
#define DHCP_OPT_SUBNET_MASK     1
#define DHCP_OPT_ROUTER          3
#define DHCP_OPT_DNS_SERVER      6
#define DHCP_OPT_REQUESTED_IP    50
#define DHCP_OPT_LEASE_TIME      51
#define DHCP_OPT_MSG_TYPE        53
#define DHCP_OPT_SERVER_ID       54
#define DHCP_OPT_END             255

//BOOTP/DHCP message structure (fixed portion = 236 bytes)
typedef struct __attribute__((packed)) {
    uint8  op;           //1=request 2=reply
    uint8  htype;        //hardware type (1=ethernet)
    uint8  hlen;         //hardware address length (6)
    uint8  hops;         //relay hops
    uint32 xid;          //transaction ID
    uint16 secs;         //seconds elapsed
    uint16 flags;        //flags (0x8000 = broadcast)
    uint32 ciaddr;       //client IP (if known)
    uint32 yiaddr;       //your IP (offered by server)
    uint32 siaddr;       //server IP
    uint32 giaddr;       //gateway IP (relay agent)
    uint8  chaddr[16];   //client hardware address
    uint8  sname[64];    //server hostname (unused)
    uint8  file[128];    //boot filename (unused)
    uint32 magic;        //DHCP magic cookie
    //options follow (variable length)
} dhcp_msg_t;

#define DHCP_OPTIONS_OFFSET  (sizeof(dhcp_msg_t))
#define DHCP_MAX_MSG_SIZE    576   //minimum UDP payload for DHCP

//initialize DHCP on an interface (blocking so waits for lease)
int dhcp_init(netif_t *nif);

#endif
