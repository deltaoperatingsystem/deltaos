#include <dm.h>
#include <mem.h>
#include <string.h>

static inline uint8 dm_pixel_bpp(uint8 format) {
    static const uint8 bpp[] = { 3, 4, 3, 4, 1 };  // RGB24, RGBA32, BGR24, BGRA32, GRAY8
    return (format < 5) ? bpp[format] : 0;
}

int dm_rle_decode(const uint8 *src, size src_size, uint8 *dst, size dst_size, uint8 bpp) {
    size src_pos = 0, dst_pos = 0;

    while (dst_pos < dst_size) {
        if (src_pos >= src_size) return -1;
        uint8 count = src[src_pos++];

        if (count == 0) return -1;

        if (src_pos + bpp > src_size) return -1;

        if ((size)count * bpp > dst_size - dst_pos) return -1;

        for (uint8 i = 0; i < count; i++) {
            memcpy(dst + dst_pos, src + src_pos, bpp);
            dst_pos += bpp;
        }
        src_pos += bpp;
    }

    return 0;
}

int dm_load_image(const void *file, size file_size, dm_image_t *out) {
    if (file_size < sizeof(dm_hdr_t)) return DM_ERR_TRUNCATED;
    if (!out) return DM_ERR_OVERFLOW;

    const dm_hdr_t *hdr = file;
    if (!hdr) return DM_ERR_TRUNCATED;

    if (hdr->magic != DM_MAGIC) return DM_ERR_MAGIC;
    if (hdr->type != DM_TYPE_IMAGE) return DM_ERR_TYPE;
    if (hdr->type >= 3) return DM_ERR_UNKNOWN_TYPE;
    if (hdr->compression >= 2) return DM_ERR_UNKNOWN_COMP;

    if (hdr->header_size < sizeof(dm_hdr_t) + sizeof(dm_img_hdr_t))
        return DM_ERR_HEADER;
    if (hdr->header_size > file_size) return DM_ERR_TRUNCATED;

    const dm_img_hdr_t *img = (const void *)((const uint8 *)file + sizeof(dm_hdr_t));
    if (!img) return DM_ERR_TRUNCATED;

    if (img->width == 0 || img->width > 16384) return DM_ERR_DIMENSIONS;
    if (img->height == 0 || img->height > 16384) return DM_ERR_DIMENSIONS;
    if (img->pixel_format > 4) return DM_ERR_PIXEL_FORMAT;
    if (img->transfer != 0) return DM_ERR_UNSUPPORTED;
    if (img->reserved[0] != 0 || img->reserved[1] != 0) return DM_ERR_RESERVED;

    if (hdr->data_offset > file_size) return DM_ERR_TRUNCATED;
    if (hdr->data_size > file_size - hdr->data_offset) return DM_ERR_TRUNCATED;

    uint8 bpp = dm_pixel_bpp(img->pixel_format);
    if (bpp == 0) return DM_ERR_PIXEL_FORMAT;

    if (img->width > SIZE_MAX / bpp) return DM_ERR_OVERFLOW;
    size row_size = (size)img->width * bpp;

    if (img->height > SIZE_MAX / row_size) return DM_ERR_OVERFLOW;
    size raw_size = row_size * img->height;

    if (hdr->raw_size != raw_size) return DM_ERR_SIZE_MISMATCH;

    out->width = img->width;
    out->height = img->height;
    out->pixel_format = img->pixel_format;
    out->bpp = bpp;
    out->pixels = malloc(raw_size);
    if (!out->pixels) return DM_ERR_OOM;

    const uint8 *data = (const uint8*)file + hdr->data_offset;

    if (hdr->compression == DM_COMP_NONE) {
        if (hdr->data_size != raw_size) {
            free(out->pixels);
            return DM_ERR_SIZE_MISMATCH;
        }
        memcpy(out->pixels, data, raw_size);
    } else if (hdr->compression == DM_COMP_RLE) {
        if (dm_rle_decode(data, hdr->data_size, out->pixels, raw_size, bpp) != 0) {
            free(out->pixels);
            return DM_ERR_DECODE;
        }
    } else {
        return DM_ERR_UNKNOWN_COMP;
    }


    return DM_OK;
}