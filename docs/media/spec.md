# Delta Media Format (DM)

## Overview

DM is the native media format for DeltaOS, designed for:

- **Fast decoding** — Simple compression, bounded memory usage
- **Safe parsing** — Explicit validation, no undefined behavior
- **Extensibility** — Reserved fields for future versions

```
┌─────────────────────────────────────────┐
│  DM Header (magic, type, dimensions)    │
├─────────────────────────────────────────┤
│  Type-specific Header                   │
│  (image/video/audio parameters)         │
├─────────────────────────────────────────┤
│  Data Section                           │
│  (pixels, samples, or frames)           │
└─────────────────────────────────────────┘
```

---

## Data Types

All multi-byte integers are **little-endian**.

| Type   | Size    | Description          |
|--------|---------|----------------------|
| `u8`   | 1 byte  | Unsigned 8-bit       |
| `u16`  | 2 bytes | Unsigned 16-bit LE   |
| `u32`  | 4 bytes | Unsigned 32-bit LE   |
| `u64`  | 8 bytes | Unsigned 64-bit LE   |

---

## DM Header

Common header for all media types

```c
struct dm_header {
    u32 magic;          // 0x444D0001 ('D' 'M' 0x00 0x01)
    u32 checksum;       // CRC32 of entire file (with this field as 0)
    u16 version;        // Format version (0x0001)
    u8  type;           // Media type (0-2, reject others)
    u8  compression;    // Compression method (0-1, reject others)
    u32 header_size;    // Total header size (common + type-specific)
    u64 data_offset;    // Offset to daa section from file start
    u64 data_size;      // Size of data section in bytes (compressed)
    u64 raw_size;       // Size of uncompressed data (for allocation)
};
```

**Size:** 40 bytes  
**Alignment:** DM files MUST be mapped at 8-byte aligned addresses.

### Magic

`0x444D0001`:
- `0x44` = 'D'
- `0x4D` = 'M'
- `0x00` = separator
- `0x01` = version

### Media Types

| Value | Name           | Description              |
|-------|----------------|------------- -------------|
| 0     | `DM_TYPE_IMAGE`| Static image             |
| 1     | `DM_TYPE_VIDEO`| Video (sequence of frames)|
| 2     | `DM_TYPE_AUDIO`| Audio stream             |

> [!IMPORTANT]
> Parsers MUST reject files with `type >= 3`. Unknown types are errors not extensions.

### Compression Methods

| Value | Name              | Description                           |
|-------|-------------------|---------------------------------------|
| 0     | `DM_COMP_NONE`    | Uncompressed (raw data)               |
| 1     | `DM_COMP_RLE`     | Run-length encoding                   |

> [!IMPORTANT]
> Parsers MUST reject files with `compression >= 2`, future versions may add LZ4.

---

## Pixel Formats

Used by image and video types.

| Value | Name              | BPP | Description                        |
|-------|-------------------|-----|------------------------------------|
| 0     | `DM_PIXEL_RGB24`  | 3   | 8-bit R, G, B (sRGB)                 |
| 1     | `DM_PIXEL_RGBA32` | 4   | 8-bit R, G, B, A (sRGB, premul)     |
| 2     | `DM_PIXEL_BGR24`  | 3   | 8-bit B, G, R (sRGB)                 |
| 3     | `DM_PIXEL_BGRA32` | 4   | 8-bit B, G, R, A (sRGB, premul)     |
| 4     | `DM_PIXEL_GRAY8`  | 1   | 8-bit grayscale (sRGB luminance)   |

> [!NOTE]
> All pixel formats use **sRGB** color space and transfer function.
> Alpha is **premultiplied**. (color values already scaled by alpha)

### Bytes Per Pixel Lookup

```c
static inline u8 dm_pixel_bpp(u8 format) {
    static const u8 bpp[] = { 3, 4, 3, 4, 1 };  // RGB24, RGBA32, BGR24, BGRA32, GRAY8
    return (format < 5) ? bpp[format] : 0;
}
```

---

## Type-Specific Headers

### Image Header (type = 0)

```c
struct dm_image_header {
    u32 width;          // Width in pixels (1-16384)
    u32 height;         // Height in pixels (1-16384)
    u8  pixel_format;   // Pixel format (0-4, reject others)
    u8  transfer;       // Transfer function (0=sRGB, reserved 1-255)
    u8  reserved[2];    // Must be zero, reject if non-zero
};
```

**Size:** 12 bytes  
**Total header (image):** 52 bytes (40 + 12)

**Validation requirements:**
- `width >= 1 && width <= 16384`
- `height >= 1 && height <= 16384`
- `pixel_format <= 4`
- `transfer == 0` (reject non-zero for v1)
- `reserved[0] == 0 && reserved[1] == 0`

**Data section:** Raw pixel data, row-major order (top-left origin).
Each row is tightly packed (no padding between rows)

---

### Video Header (type = 1)

```c
struct dm_video_header {
    u32 width;          // Frame width in pixels (1-16384)
    u32 height;         // Frame height in pixels (1-16384)
    u32 frame_count;    // Total number of frames (0 = unknown/streaming)
    u32 fps_num;        // Framerate numerator (1-1000000)
    u32 fps_den;        // Framerate denominator (1-1000000, e.g., 1001 for 23.976)
    u8  pixel_format;   // Pixel format (0-4, reject others)
    u8  flags;          // Video flags (see below)
    u8  transfer;       // Transfer function (0=sRGB)
    u8  reserved;       // Must be zero
};
```

**Size:** 24 bytes  
**Total header (video):** 64 bytes (40 + 24)

**Validation requirements:**
- `fps_num >= 1 && fps_num <= 1000000`
- `fps_den >= 1 && fps_den <= 1000000`

> [!IMPORTANT]
> `fps_den` MUST be >= 1. Division by zero is undefined behavior.

**Video Flags:**

| Bit | Name              | Description                      |
|-----|-------------------|----------------------------------|
| 0   | `DM_VID_LOOP`     | Video should loop                |

**Data section:** Frames stored sequentially. Each frame is:
- `width * height * bpp` bytes (uncompressed)
- Or RLE-compressed (each frame compressed independently)

> [!WARNING]
> No intra-frame seeking. To decode frame N, all frames 0..N-1 must be decoded.
> This format is NOT suitable for long-form video content.

---

### Audio Header (type = 2)

```c
struct dm_audio_header {
    u32 sample_rate;    // Samples per second (8000-192000)
    u32 sample_count;   // Total samples per channel (0 = streaming)
    u8  channels;       // Number of channels (1=mono, 2=stereo)
    u8  bits_per_sample;// 16 or 32 only (reject others)
    u8  format;         // Sample format (see below)
    u8  reserved;       // Must be zero
};
```

**Size:** 12 bytes  
**Total header (audio):** 52 bytes (40 + 12)

**Sample Formats:**

| Value | Name            | Description                    |
|-------|-----------------|--------------------------------|
| 0     | `DM_AUDIO_PCM`  | Signed PCM (little-endian)     |
| 1     | `DM_AUDIO_FLOAT`| IEEE 754 float (32-bit only)   |

**Validation requirements:**
- `channels == 1 || channels == 2` (v1 supports mono/stereo only)
- `bits_per_sample == 16 || bits_per_sample == 32`
- `sample_rate >= 8000 && sample_rate <= 192000`

> [!IMPORTANT]
> `bits_per_sample` MUST be 16 or 32. 8-bit and 24-bit are NOT supported
> (8-bit has poor quality ans 24-bit causes alignment issues).
>
> `channels` > 2 is reserved for future versions. Reject for v1.

**Data section:** Interleaved samples.
For stereo: `L0 R0 L1 R1 L2 R2 ...`

**Channel layout:**
- 1 channel: Mono
- 2 channels: Left, Right

---

## RLE Compression

Run-length encoding operates on pixels. Format:

```
[count][pixel_data]...
```

Where:
- `count` (u8): Number of times to repeat (1-255, **0 is invalid**)
- `pixel_data`: One pixel in the file's pixel format

**Example (RGBA32):**
```
Input:  AAAA AAAA BBBB CCCC CCCC CCCC  (6 pixels, 24 bytes)
RLE:    04 AAAA 01 BBBB 03 CCCC        (15 bytes)
```

**Decoder with full validation:**
```c
int dm_rle_decode(const u8 *src, size_t src_size,
                  u8 *dst, size_t dst_size, u8 bpp) {
    size_t src_pos = 0, dst_pos = 0;
    
    while (dst_pos < dst_size) {
        // Check bounds on count byte
        if (src_pos >= src_size) return -1;
        u8 count = src[src_pos++];
        
        // Reject count == 0 (invalid)
        if (count == 0) return -1;
        
        // Check bounds on pixel data
        if (src_pos + bpp > src_size) return -1;
        
        // Check output won't overflow
        if ((size_t)count * bpp > dst_size - dst_pos) return -1;
        
        // Copy pixel 'count' times
        for (u8 i = 0; i < count; i++) {
            memcpy(dst + dst_pos, src + src_pos, bpp);
            dst_pos += bpp;
        }
        src_pos += bpp;
    }
    
    return 0;
}
```

---

## Loading Algorithm

### Safe Image Loading

```c
int dm_load_image(const void *file, size_t file_size, dm_image_t *out,
                  void *(*alloc)(size_t), void (*dealloc)(void *)) {
    if (file_size < sizeof(struct dm_header)) return DM_ERR_TRUNCATED;
    
    const struct dm_header *hdr = file;
    
    // Verify magic and type
    if (hdr->magic != 0x444D0001) return DM_ERR_MAGIC;
    if (hdr->type != DM_TYPE_IMAGE) return DM_ERR_TYPE;
    if (hdr->type >= 3) return DM_ERR_UNKNOWN_TYPE;
    if (hdr->compression >= 2) return DM_ERR_UNKNOWN_COMP;
    
    // Validate header_size
    if (hdr->header_size < sizeof(*hdr) + sizeof(struct dm_image_header))
        return DM_ERR_HEADER;
    if (hdr->header_size > file_size) return DM_ERR_TRUNCATED;
    
    const struct dm_image_header *img = (const void *)((const u8 *)file + sizeof(*hdr));
    
    // Validate dimensions
    if (img->width == 0 || img->width > 16384) return DM_ERR_DIMENSIONS;
    if (img->height == 0 || img->height > 16384) return DM_ERR_DIMENSIONS;
    if (img->pixel_format > 4) return DM_ERR_PIXEL_FORMAT;
    if (img->transfer != 0) return DM_ERR_UNSUPPORTED;
    if (img->reserved[0] != 0 || img->reserved[1] != 0) return DM_ERR_RESERVED;
    
    // Validate data bounds
    if (hdr->data_offset > file_size) return DM_ERR_TRUNCATED;
    if (hdr->data_size > file_size - hdr->data_offset) return DM_ERR_TRUNCATED;
    
    // OVERFLOW-SAFE size calculation
    u8 bpp = dm_pixel_bpp(img->pixel_format);
    if (bpp == 0) return DM_ERR_PIXEL_FORMAT;  // Unknown format
    
    if (img->width > SIZE_MAX / bpp) return DM_ERR_OVERFLOW;
    size_t row_size = (size_t)img->width * bpp;
    
    // row_size == 0 is impossible here (width >= 1, bpp >= 1)
    if (img->height > SIZE_MAX / row_size) return DM_ERR_OVERFLOW;
    size_t raw_size = row_size * img->height;
    
    // Validate raw_size matches header
    if (hdr->raw_size != raw_size) return DM_ERR_SIZE_MISMATCH;
    
    out->width = img->width;
    out->height = img->height;
    out->bpp = bpp;
    out->pixels = alloc(raw_size);
    if (!out->pixels) return DM_ERR_OOM;
    
    const u8 *data = (const u8 *)file + hdr->data_offset;
    
    if (hdr->compression == DM_COMP_NONE) {
        if (hdr->data_size != raw_size) {
            dealloc(out->pixels);
            return DM_ERR_SIZE_MISMATCH;
        }
        memcpy(out->pixels, data, raw_size);
    } else if (hdr->compression == DM_COMP_RLE) {
        if (dm_rle_decode(data, hdr->data_size, out->pixels, raw_size, bpp) != 0) {
            dealloc(out->pixels);
            return DM_ERR_DECODE;
        }
    }
    
    return DM_OK;
}
```

### Error Codes

| Value | Name                  | Description                    |
|-------|-----------------------|--------------------------------|
| 0     | `DM_OK`               | Success                        |
| -1    | `DM_ERR_TRUNCATED`    | File too small                 |
| -2    | `DM_ERR_MAGIC`        | Invalid magic number           |
| -3    | `DM_ERR_TYPE`         | Wrong media type               |
| -4    | `DM_ERR_UNKNOWN_TYPE` | Unknown type (reject)          |
| -5    | `DM_ERR_UNKNOWN_COMP` | Unknown compression (reject)   |
| -6    | `DM_ERR_HEADER`       | Header size invalid            |
| -7    | `DM_ERR_DIMENSIONS`   | Invalid dimensions             |
| -8    | `DM_ERR_PIXEL_FORMAT` | Unknown pixel format           |
| -9    | `DM_ERR_UNSUPPORTED`  | Unsupported feature (reserved) |
| -10   | `DM_ERR_RESERVED`     | Non-zero reserved field        |
| -11   | `DM_ERR_OVERFLOW`     | Size calculation overflow      |
| -12   | `DM_ERR_SIZE_MISMATCH`| Data size doesn't match        |
| -13   | `DM_ERR_OOM`          | Allocation failed              |
| -14   | `DM_ERR_DECODE`       | Decompression error            |

---

## Comparison to Other Formats (as always)

| Feature          | DM      | PNG     | BMP     | stb_image |
|------------------|---------|---------|---------|-----------|
| Header size      | 40-64B  | Variable| 54B+    | N/A       |
| Dependencies     | None    | zlib    | None    | None      |
| Compression      | RLE     | DEFLATE | RLE     | N/A       |
| Alpha channel    | Yes     | Yes     | Limited | Yes       |
| Code complexity  | ~200 LOC| ~8K LOC | ~100 LOC| ~7K LOC   |

---

## Security Checklist

Decoders MUST implement ALL of these checks:

- [ ] `magic == 0x444D0001`
- [ ] `type < 3` (reject unknown)
- [ ] `compression < 2` (reject unknown)
- [ ] `header_size >= minimum for type`
- [ ] `header_size <= file_size`
- [ ] `data_offset + data_size <= file_size`
- [ ] `width >= 1 && width <= 16384`
- [ ] `height >= 1 && height <= 16384`
- [ ] `pixel_format` in valid range
- [ ] `reserved` fields are zero
- [ ] Verify `fps_num >= 1 && fps_den >= 1` before division
- [ ] Verify `channels <= 2` (v1 limit)
- [ ] Verify `sample_rate <= 192000`
- [ ] Verify `data_offset` is 8-byte aligned
- [ ] Verify `raw_size` matches computed size
- [ ] RLE count != 0
- [ ] RLE input bounds checked
- [ ] RLE output bounds checked

> [!CAUTION]
> Checksum validation is NOT optional. DM files, may come from untrusted sources
> (like downloads, USB drives). Always verify CRC32 before parsing.

---

## Notes

- **sRGB only:** All colors are sRGB. No colorspace negotiation.
- **No metadata:** Use separate files for EXIF/XMP.
- **No streaming video:** For boot splashes and icons only.
