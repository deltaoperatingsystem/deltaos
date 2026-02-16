#ifndef NET_NETBUF_H
#define NET_NETBUF_H

#include <arch/types.h>

/*
 * network buffer - simple sk_buff-like structure for packet handling
 *
 * layout: [headroom][data...len...][tailroom]
 *         ^buf      ^data                  ^buf+capacity
 *
 * headroom allows prepending headers (ethernet, IP) without copying
 */

#define NETBUF_DEFAULT_HEADROOM 64  //enough for eth+ip+tcp headers
#define NETBUF_DEFAULT_SIZE     2048

typedef struct netbuf {
    uint8 *buf;       //raw backing buffer
    uint8 *data;      //start of packet data
    size len;          //length of packet data
    size capacity;     //total buffer capacity
    
    struct netbuf *next;  //for linked-list queues
} netbuf_t;

//allocate a new netbuf with given capacity and headroom
netbuf_t *netbuf_alloc(size capacity);

//free a netbuf
void netbuf_free(netbuf_t *nb);

//reserve headroom (call before pushing data)
void netbuf_reserve(netbuf_t *nb, size headroom);

//push data to the front (grow packet toward head, e.g. prepend header)
//returns pointer to the new data area
void *netbuf_push(netbuf_t *nb, size len);

//pull data from the front (shrink packet, e.g. strip header)
//returns pointer to what was the front
void *netbuf_pull(netbuf_t *nb, size len);

//put data at the tail (append data to packet)
//returns pointer to the new data area
void *netbuf_put(netbuf_t *nb, size len);

#endif
