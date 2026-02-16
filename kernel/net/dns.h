#ifndef NET_DNS_H
#define NET_DNS_H

#include <arch/types.h>


#define DNS_SERVER_PORT 53

//DNS header structure
typedef struct __attribute__((packed)) {
    uint16 id;          //transaction ID
    uint16 flags;       //flags (QR, Opcode, AA, TC, RD, RA, Z, RCODE)
    uint16 qdcount;     //number of questions
    uint16 ancount;     //number of answers
    uint16 nscount;     //number of authority records
    uint16 arcount;     //number of additional records
} dns_header_t;

//DNS flags bits (big-endian)
#define DNS_FLAG_QR      0x8000  //0=query 1=response
#define DNS_FLAG_RD      0x0100  //recursion desired
#define DNS_FLAG_RA      0x0080  //recursion available
#define DNS_FLAG_RCODE   0x000F  //response code (0=no error)

//DNS types
#define DNS_TYPE_A       1       //IPv4 address
#define DNS_TYPE_NS      2
#define DNS_TYPE_CNAME   5
#define DNS_TYPE_PTR     12
#define DNS_TYPE_MX      15
#define DNS_TYPE_TXT     16

//DNS classes
#define DNS_CLASS_IN     1       //internet

//resolve a hostname to an IPv4 address (blocking)
//returns 0 on success fills ip_out. returns < 0 on error.
int dns_resolve(const char *hostname, uint32 *ip_out);

#endif
