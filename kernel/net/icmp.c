#include <net/icmp.h>
#include <net/ipv4.h>
#include <net/ethernet.h>
#include <net/endian.h>
#include <lib/io.h>
#include <lib/string.h>

void icmp_recv(netif_t *nif, uint32 src_ip, void *data, size len) {
    if (len < sizeof(icmp_header_t)) return;
    
    icmp_header_t *icmp = (icmp_header_t *)data;
    
    if (icmp->type == ICMP_TYPE_ECHO_REQUEST && icmp->code == 0) {
        printf("[icmp] Echo request from ");
        net_print_ip(src_ip);
        printf(", replying\n");
        
        //build echo reply reuse the payload
        uint8 reply[ETH_MTU];
        if (len > ETH_MTU) return;
        
        icmp_header_t *rep = (icmp_header_t *)reply;
        rep->type = ICMP_TYPE_ECHO_REPLY;
        rep->code = 0;
        rep->checksum = 0;
        rep->id = icmp->id;
        rep->sequence = icmp->sequence;
        
        //copy any echo data after the header
        if (len > sizeof(icmp_header_t)) {
            memcpy(reply + sizeof(icmp_header_t),
                   (uint8 *)data + sizeof(icmp_header_t),
                   len - sizeof(icmp_header_t));
        }
        
        rep->checksum = ipv4_checksum(reply, len);
        
        ipv4_send(nif, src_ip, IPPROTO_ICMP, reply, len);
    } else if (icmp->type == ICMP_TYPE_ECHO_REPLY) {
        printf("[icmp] Echo reply from ");
        net_print_ip(src_ip);
        printf(" seq=%u\n", ntohs(icmp->sequence));
    }
}

int icmp_send_echo(netif_t *nif, uint32 dst_ip, uint16 id, uint16 seq,
                   const void *payload, size payload_len) {
    uint8 packet[ETH_MTU];
    size total = sizeof(icmp_header_t) + payload_len;
    if (total > ETH_MTU) return -1;
    
    icmp_header_t *icmp = (icmp_header_t *)packet;
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(id);
    icmp->sequence = htons(seq);
    
    if (payload_len > 0 && payload) {
        memcpy(packet + sizeof(icmp_header_t), payload, payload_len);
    }
    
    icmp->checksum = ipv4_checksum(packet, total);
    
    return ipv4_send(nif, dst_ip, IPPROTO_ICMP, packet, total);
}
