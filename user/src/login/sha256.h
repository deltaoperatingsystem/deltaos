#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>

void sha256(const void *data, size_t len, uint8_t digest[32]);
void sha256_to_hex(const uint8_t digest[32], char out_hex[65]);
int hex_to_bin(const char *hex, uint8_t *out, size_t outlen);
int ct_memcmp(const void *a, const void *b, size_t len);

#endif