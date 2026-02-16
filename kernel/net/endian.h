#ifndef NET_ENDIAN_H
#define NET_ENDIAN_H

#include <arch/types.h>

//x86 is little-endian network byte order is big-endian
//swap helpers
//most modern archs are little-endian but this could be a little abstracted

static inline uint16 htons(uint16 x) {
    return (x >> 8) | (x << 8);
}

static inline uint16 ntohs(uint16 x) {
    return htons(x);
}

static inline uint32 htonl(uint32 x) {
    return ((x >> 24) & 0x000000FF) |
           ((x >>  8) & 0x0000FF00) |
           ((x <<  8) & 0x00FF0000) |
           ((x << 24) & 0xFF000000);
}

static inline uint32 ntohl(uint32 x) {
    return htonl(x);
}

#endif
