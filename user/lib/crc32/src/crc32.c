#include <crc32.h>

//ISO 3309 / ITU-T V.42 reflected CRC-32 table (sarwate algorithm)
//polynomial: 0xEDB88320 (bit-reversed 0x04C11DB7)
static uint32 crc32_table[256];
static int    crc32_ready = 0;

static void crc32_build_table(void) {
    if (crc32_ready) return;
    for (uint32 i = 0; i < 256; i++) {
        uint32 c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

uint32 crc32_update(uint32 prev, const void *data, size len) {
    crc32_build_table();
    uint32 crc = prev;
    const uint8 *p = (const uint8 *)data;
    for (size i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32 crc32(const void *data, size len) {
    return crc32_update(CRC32_INIT, data, len) ^ CRC32_FINAL;
}
