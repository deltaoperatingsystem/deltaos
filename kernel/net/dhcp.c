#include <net/dhcp.h>
#include <net/udp.h>
#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/time.h>
#include <arch/cpu.h>
#include <arch/timer.h>

/*
 *DHCP client, 4-step handshake:
 * 1. DISCOVER (broadcast)
 * 2. OFFER    (server -> client)
 * 3. REQUEST  (broadcast)
 * 4. ACK      (server -> client)
 *
 * we implement this as a blocking state machine called during boot
 *the UDP callback stores the offer/ack and the main loop polls for it
 */

//state machine
typedef enum {
    DHCP_STATE_INIT,
    DHCP_STATE_DISCOVERING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_ERROR
} dhcp_state_t;

//DHCP context
static struct {
    netif_t      *nif;
    uint8         chaddr[16];
    dhcp_state_t  state;
    uint32        xid;          //transaction ID
    uint32        offered_ip;
    uint32        server_ip;
    uint32        subnet_mask;
    uint32        gateway;
    uint32        dns_server;
    uint32        lease_time;
} dhcp_ctx;

//parse a single DHCP option, return pointer to next option or NULL
static const uint8 *dhcp_parse_option(const uint8 *opt, const uint8 *end,
                                       uint8 *type_out, uint8 *len_out, const uint8 **data_out) {
    if (opt >= end) return NULL;
    uint8 type = opt[0];
    if (type == DHCP_OPT_END) return NULL;
    if (opt + 1 >= end) return NULL;
    uint8 len = opt[1];
    if (opt + 2 + len > end) {
        return NULL;
    }

    *type_out = type;
    *len_out = len;
    *data_out = opt + 2;
    return opt + 2 + len;
}

static uint8 *dhcp_append_client_id(uint8 *opt, const uint8 mac[6]) {
    *opt++ = 61;  //client identifier
    *opt++ = 7;
    *opt++ = 1;   //ethernet
    memcpy(opt, mac, 6);
    return opt + 6;
}

static uint8 *dhcp_append_common_request_options(uint8 *opt) {
    static const uint8 prl[] = {
        DHCP_OPT_SUBNET_MASK,
        DHCP_OPT_ROUTER,
        DHCP_OPT_DNS_SERVER,
        DHCP_OPT_LEASE_TIME,
    };

    *opt++ = 55;  //parameter request list
    *opt++ = sizeof(prl);
    memcpy(opt, prl, sizeof(prl));
    opt += sizeof(prl);

    *opt++ = 12;  //hostname
    *opt++ = 7;
    memcpy(opt, "DeltaOS", 7);
    opt += 7;

    return opt;
}

//build and send a DHCP DISCOVER
static void dhcp_send_discover(netif_t *nif) {
    uint8 buf[DHCP_MAX_MSG_SIZE];
    memset(buf, 0, sizeof(buf));

    dhcp_msg_t *msg = (dhcp_msg_t *)buf;
    msg->op    = DHCP_OP_REQUEST;
    msg->htype = DHCP_HTYPE_ETH;
    msg->hlen  = 6;
    msg->hops  = 0;
    msg->xid   = htonl(dhcp_ctx.xid);
    msg->secs  = 0;
    msg->flags = htons(0x8000);  //broadcast flag
    msg->ciaddr = 0;
    msg->yiaddr = 0;
    msg->siaddr = 0;
    msg->giaddr = 0;
    memcpy(msg->chaddr, nif->mac, 6);
    msg->magic = htonl(DHCP_MAGIC_COOKIE);

    //add options
    uint8 *opt = buf + DHCP_OPTIONS_OFFSET;

    //option 53: DHCP Message Type = DISCOVER
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_DISCOVER;

    opt = dhcp_append_client_id(opt, nif->mac);
    opt = dhcp_append_common_request_options(opt);

    //end
    *opt++ = DHCP_OPT_END;

    size msg_len = (size)(opt - buf);
    if (msg_len < DHCP_MIN_MSG_SIZE) {
        msg_len = DHCP_MIN_MSG_SIZE;
    }
    (void)udp_send_no_checksum(nif, 0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, msg_len);
}

//build and send a DHCP REQUEST
static void dhcp_send_request(netif_t *nif) {
    uint8 buf[DHCP_MAX_MSG_SIZE];
    memset(buf, 0, sizeof(buf));

    dhcp_msg_t *msg = (dhcp_msg_t *)buf;
    msg->op    = DHCP_OP_REQUEST;
    msg->htype = DHCP_HTYPE_ETH;
    msg->hlen  = 6;
    msg->hops  = 0;
    msg->xid   = htonl(dhcp_ctx.xid);
    msg->secs  = 0;
    msg->flags = htons(0x8000);
    msg->ciaddr = 0;
    msg->yiaddr = 0;
    msg->siaddr = 0;
    msg->giaddr = 0;
    memcpy(msg->chaddr, nif->mac, 6);
    msg->magic = htonl(DHCP_MAGIC_COOKIE);

    //add options
    uint8 *opt = buf + DHCP_OPTIONS_OFFSET;

    //option 53: DHCP Message Type = REQUEST
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_REQUEST;

    opt = dhcp_append_client_id(opt, nif->mac);

    //option 50: Requested IP Address
    *opt++ = DHCP_OPT_REQUESTED_IP;
    *opt++ = 4;
    memcpy(opt, &dhcp_ctx.offered_ip, 4);
    opt += 4;

    //option 54: Server Identifier
    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    memcpy(opt, &dhcp_ctx.server_ip, 4);
    opt += 4;

    opt = dhcp_append_common_request_options(opt);

    //end
    *opt++ = DHCP_OPT_END;

    size msg_len = (size)(opt - buf);
    if (msg_len < DHCP_MIN_MSG_SIZE) {
        msg_len = DHCP_MIN_MSG_SIZE;
    }
    (void)udp_send_no_checksum(nif, 0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, msg_len);
}

//UDP callback for port 68 (DHCP client)
static void dhcp_recv_handler(netif_t *nif, uint32 src_ip, uint16 src_port,
                               const void *data, size len, void *ctx) {
    (void)src_port;
    if (ctx != (void *)&dhcp_ctx) return;
    if (!dhcp_ctx.nif || nif != dhcp_ctx.nif) return;

    if (len < sizeof(dhcp_msg_t)) return;

    const dhcp_msg_t *msg = (const dhcp_msg_t *)data;

    //verify this is a reply for our transaction
    if (msg->op != DHCP_OP_REPLY) return;
    if (msg->htype != DHCP_HTYPE_ETH || msg->hlen != 6) return;
    if (ntohl(msg->xid) != dhcp_ctx.xid) return;
    if (msg->magic != htonl(DHCP_MAGIC_COOKIE)) return;
    if (memcmp(msg->chaddr, dhcp_ctx.chaddr, 6) != 0) return;

    //parse options
    const uint8 *opt = (const uint8 *)data + DHCP_OPTIONS_OFFSET;
    const uint8 *end = (const uint8 *)data + len;

    uint8 msg_type = 0;
    uint32 server_id = 0;
    uint32 subnet = 0;
    uint32 router = 0;
    uint32 dns = 0;
    uint32 lease = 0;

    while (opt < end) {
        uint8 type, olen;
        const uint8 *odata;
        opt = dhcp_parse_option(opt, end, &type, &olen, &odata);
        if (!opt) break;

        switch (type) {
            case DHCP_OPT_MSG_TYPE:
                if (olen >= 1) msg_type = odata[0];
                break;
            case DHCP_OPT_SUBNET_MASK:
                if (olen >= 4) memcpy(&subnet, odata, 4);
                break;
            case DHCP_OPT_ROUTER:
                if (olen >= 4) memcpy(&router, odata, 4);
                break;
            case DHCP_OPT_DNS_SERVER:
                if (olen >= 4) memcpy(&dns, odata, 4);
                break;
            case DHCP_OPT_LEASE_TIME:
                if (olen >= 4) {
                    uint32 raw;
                    memcpy(&raw, odata, 4);
                    lease = ntohl(raw);
                }
                break;
            case DHCP_OPT_SERVER_ID:
                if (olen >= 4) memcpy(&server_id, odata, 4);
                break;
        }
    }

    if (dhcp_ctx.state == DHCP_STATE_DISCOVERING && msg_type == DHCP_OFFER) {
        dhcp_ctx.offered_ip  = msg->yiaddr;
        dhcp_ctx.server_ip   = server_id ? server_id : src_ip;
        dhcp_ctx.subnet_mask = subnet;
        dhcp_ctx.gateway     = router;
        dhcp_ctx.dns_server  = dns;
        dhcp_ctx.lease_time  = lease;
        dhcp_ctx.state       = DHCP_STATE_REQUESTING;

    }
    else if (dhcp_ctx.state == DHCP_STATE_REQUESTING && msg_type == DHCP_ACK) {
        dhcp_ctx.offered_ip  = msg->yiaddr;
        if (subnet) dhcp_ctx.subnet_mask = subnet;
        if (router) dhcp_ctx.gateway     = router;
        if (dns)    dhcp_ctx.dns_server  = dns;
        if (lease)  dhcp_ctx.lease_time  = lease;
        dhcp_ctx.state = DHCP_STATE_BOUND;
    }
    else if (msg_type == DHCP_NAK) {
        dhcp_ctx.state = DHCP_STATE_ERROR;
    }
}

int dhcp_init(netif_t *nif) {
    memset(&dhcp_ctx, 0, sizeof(dhcp_ctx));
    dhcp_ctx.nif   = nif;
    dhcp_ctx.state = DHCP_STATE_INIT;
    memcpy(dhcp_ctx.chaddr, nif->mac, 6);

    //generate a pseudo-random transaction ID from MAC + ticks
    dhcp_ctx.xid = (uint32)nif->mac[2] << 24 | (uint32)nif->mac[3] << 16 |
                   (uint32)nif->mac[4] << 8  | (uint32)nif->mac[5];
    dhcp_ctx.xid ^= (uint32)arch_timer_get_ticks();

    //bind UDP port 68 for incoming DHCP replies
    if (udp_bind(DHCP_CLIENT_PORT, dhcp_recv_handler, &dhcp_ctx) < 0) return -1;

    //phase 1: DISCOVER -> OFFER
    dhcp_ctx.state = DHCP_STATE_DISCOVERING;

    uint32 freq = arch_timer_getfreq();
    if (freq == 0) freq = 1000;

    for (int attempt = 0; attempt < 4; attempt++) {
        dhcp_send_discover(nif);

        //wait up to 2 seconds using the real system timer
        uint64 start = arch_timer_get_ticks();
        uint32 timeout_ticks = freq * 2;
        while (arch_timer_get_ticks() - start < timeout_ticks) {
            net_poll();
            if (dhcp_ctx.state != DHCP_STATE_DISCOVERING) goto got_offer;
            sleep(1);
        }
    }

    udp_unbind(DHCP_CLIENT_PORT);
    return -1;

got_offer:
    if (dhcp_ctx.state == DHCP_STATE_ERROR) goto fallback;

    //phase 2: REQUEST -> ACK
    for (int attempt = 0; attempt < 4; attempt++) {
        dhcp_send_request(nif);

        //wait up to 2 seconds using the real system timer
        uint64 start = arch_timer_get_ticks();
        uint32 timeout_ticks = freq * 2;
        while (arch_timer_get_ticks() - start < timeout_ticks) {
            net_poll();
            if (dhcp_ctx.state == DHCP_STATE_BOUND) goto bound;
            if (dhcp_ctx.state == DHCP_STATE_ERROR) goto fallback;
            sleep(1);
        }
    }

fallback:
    udp_unbind(DHCP_CLIENT_PORT);
    return -1;

bound:
    //apply the lease to the interface
    nif->ip_addr     = dhcp_ctx.offered_ip;
    nif->subnet_mask = dhcp_ctx.subnet_mask ? dhcp_ctx.subnet_mask : ip_make(255, 255, 255, 0);
    nif->gateway     = dhcp_ctx.gateway;
    nif->dns_server  = dhcp_ctx.dns_server;

    udp_unbind(DHCP_CLIENT_PORT);
    return 0;
}
