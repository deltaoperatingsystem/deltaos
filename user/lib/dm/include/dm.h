#ifndef DM_COMMON_H
#define DM_COMMON_H

#include <types.h>

typedef struct {
    uint32 width, height;
    uint8 bpp;
    uint8 *pixels;
} dm_image_t;

typedef struct dm_hdr {
    uint32 magic;          // 0x444D0001 ('D' 'M' 0x00 0x01)
    uint32 checksum;       // CRC32 of entire file (with this field as 0)
    uint16 version;        // Format version (0x0001)
    uint8  type;           // Media type (0-2, reject others)
    uint8  compression;    // Compression method (0-1, reject others)
    uint32 header_size;    // Total header size (common + type-specific)
    uint64 data_offset;    // Offset to daa section from file start
    uint64 data_size;      // Size of data section in bytes (compressed)
    uint64 raw_size;       // Size of uncompressed data (for allocation)
} dm_hdr_t;

typedef struct dm_img_hdr {
    uint32 width, height;   // in pixels (1-16384)
    uint8 pixel_format;     // reject if not 0-4 inclusive
    uint8 transfer;         // 0=sRGB, others reserved
    uint8 reserved[2];      // must be 0
} dm_img_hdr_t;

// magic
#define DM_MAGIC    0x444D0001

//media types
#define DM_TYPE_IMAGE   0
#define DM_TYPE_VIDEO   1
#define DM_TYPE_AUDIO   2

//compression methods
#define DM_COMP_NONE    0
#define DM_COMP_RLE     1

//pixel formats
#define DM_PIXEL_RGB24  0
#define DM_PIXEL_RGBA32 1
#define DM_PIXEL_BGR24  2
#define DM_PIXEL_BGRA32 3
#define DM_PIXEL_GRAY8  4

// error codes
#define DM_OK                   0
#define DM_ERR_TRUNCATED        1
#define DM_ERR_MAGIC            2
#define DM_ERR_TYPE             3
#define DM_ERR_UNKNOWN_TYPE     4
#define DM_ERR_UNKNOWN_COMP     5
#define DM_ERR_DIMENSIONS       6
#define DM_ERR_PIXEL_FORMAT     7
#define DM_ERR_UNSUPPORTED      8
#define DM_ERR_RESERVED         9
#define DM_ERR_OVERFLOW         10
#define DM_ERR_SIZE_MISMATCH    11
#define DM_ERR_OOM              12
#define DM_ERR_DECODE           13
#define DM_ERR_HEADER           14

// the actual functions
int dm_load_image(const void *file, size file_size, dm_image_t *out);

#endif