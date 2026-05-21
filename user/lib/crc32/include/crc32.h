#ifndef CRC32_H
#define CRC32_H

#include <types.h>

//compute standard ISO 3309 / ITU-T V.42 CRC-32 over a buffer
//returns the CRC-32 checksum
uint32 crc32(const void *data, size len);

//incremental variant: pass a previous CRC-32 value as `prev` to chain
//multiple calls, start with prev=0xFFFFFFFF, XOR the final result with
//0xFFFFFFFF yourself, or just call crc32() which does both for you
uint32 crc32_update(uint32 prev, const void *data, size len);

#define CRC32_INIT  0xFFFFFFFFu
#define CRC32_FINAL 0xFFFFFFFFu

#endif
