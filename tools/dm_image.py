#!/usr/bin/env python3
"""
img2dm.py - convert image -> Delta Media (DM) image (v1)

Usage:
    python img2dm.py input.png -o output.dm [--format auto|rgb24|rgba32|bgr24|bgra32|gray8] [--comp none|rle]

Notes:
- Requires Pillow: pip install pillow
- Ensures data_offset is 8-byte aligned and header_size includes that padding.
- Alpha is premultiplied for RGBA/BGRA.
"""

import argparse
import struct
import binascii
from PIL import Image

# DM constants
DM_MAGIC = 0x444D0001
DM_VERSION = 0x0001

DM_TYPE_IMAGE = 0

DM_COMP_NONE = 0
DM_COMP_RLE = 1

DM_PIXEL_RGB24  = 0
DM_PIXEL_RGBA32 = 1
DM_PIXEL_BGR24  = 2
DM_PIXEL_BGRA32 = 3
DM_PIXEL_GRAY8  = 4

PIX_FORMATS = {
    "rgb24": DM_PIXEL_RGB24,
    "rgba32": DM_PIXEL_RGBA32,
    "bgr24": DM_PIXEL_BGR24,
    "bgra32": DM_PIXEL_BGRA32,
    "gray8": DM_PIXEL_GRAY8,
    "auto": None,
}

def dm_pixel_bpp(fmt):
    bpp = [3,4,3,4,1]
    return bpp[fmt] if 0 <= fmt < len(bpp) else 0

def align_up(x, a):
    return ((x + (a - 1)) // a) * a

def premultiply_alpha(pixels):
    # pixels: bytearray of RGBA triples (r,g,b,a) groups
    # mutate in place and return
    for i in range(0, len(pixels), 4):
        r = pixels[i]
        g = pixels[i+1]
        b = pixels[i+2]
        a = pixels[i+3]
        if a == 255:
            continue
        # premultiply: round((c * a) / 255)
        pixels[i]   = (r * a + 127) // 255
        pixels[i+1] = (g * a + 127) // 255
        pixels[i+2] = (b * a + 127) // 255
    return pixels

def rle_encode(src_pixels, bpp):
    """
    RLE encoding per DM spec: [count (u8)][pixel_data bytes...] ...
    count in 1..255. Pixel-sized groups repeat.
    Returns bytes object.
    """
    if bpp <= 0:
        raise ValueError("invalid bpp for rle")

    out = bytearray()
    src = memoryview(src_pixels)
    i = 0
    total_pixels = len(src) // bpp

    # helper to fetch pixel tuple bytes for index j
    def get_pixel(j):
        off = j * bpp
        return bytes(src[off:off + bpp])

    while i < total_pixels:
        cur = get_pixel(i)
        run = 1
        j = i + 1
        # accumulate run up to 255
        while j < total_pixels and run < 255 and get_pixel(j) == cur:
            run += 1
            j += 1
        out.append(run)           # count (u8) (non-zero)
        out.extend(cur)           # pixel data
        i += run
    return bytes(out)

def image_to_pixel_bytes(img: Image.Image, target_fmt: int):
    """
    Convert Pillow image to raw bytes in desired DM pixel format.
    Returns (width, height, bpp, pixels_bytes)
    """
    # normalize modes
    mode = img.mode
    if target_fmt is None:
        # auto choose
        if mode in ("RGBA", "LA"):
            target_fmt = DM_PIXEL_RGBA32
        elif mode in ("RGB",):
            target_fmt = DM_PIXEL_RGB24
        elif mode in ("L",):
            target_fmt = DM_PIXEL_GRAY8
        else:
            # fallback to RGBA to preserve alpha
            target_fmt = DM_PIXEL_RGBA32

    if target_fmt == DM_PIXEL_GRAY8:
        im = img.convert("L")
        pixels = im.tobytes()
        return im.width, im.height, 1, pixels

    if target_fmt == DM_PIXEL_RGB24:
        im = img.convert("RGB")
        pixels = im.tobytes()  # R G B
        return im.width, im.height, 3, pixels

    if target_fmt == DM_PIXEL_BGR24:
        im = img.convert("RGB")
        raw = bytearray(im.tobytes())  # R G B repeated
        # swap R<->B per pixel
        for i in range(0, len(raw), 3):
            raw[i], raw[i+2] = raw[i+2], raw[i]
        return im.width, im.height, 3, bytes(raw)

    if target_fmt == DM_PIXEL_RGBA32:
        im = img.convert("RGBA")
        raw = bytearray(im.tobytes())  # R G B A
        # premultiply alpha as spec requires premultiplied alpha
        premultiply_alpha(raw)
        return im.width, im.height, 4, bytes(raw)

    if target_fmt == DM_PIXEL_BGRA32:
        im = img.convert("RGBA")
        raw = bytearray(im.tobytes())  # R G B A
        premultiply_alpha(raw)
        # reorder to B G R A
        for i in range(0, len(raw), 4):
            r, g, b, a = raw[i], raw[i+1], raw[i+2], raw[i+3]
            raw[i]   = b
            raw[i+1] = g
            raw[i+2] = r
            raw[i+3] = a
        return im.width, im.height, 4, bytes(raw)

    raise ValueError("unsupported target_fmt")

def build_dm_image(width, height, pixel_format, compression, pixel_bytes):
    # validations per spec
    if not (1 <= width <= 16384):
        raise ValueError("width out of range")
    if not (1 <= height <= 16384):
        raise ValueError("height out of range")
    if pixel_format < 0 or pixel_format > 4:
        raise ValueError("invalid pixel format")
    if compression not in (DM_COMP_NONE, DM_COMP_RLE):
        raise ValueError("invalid compression")

    bpp = dm_pixel_bpp(pixel_format)
    if bpp == 0:
        raise ValueError("bad bpp")
    raw_size = width * height * bpp
    if len(pixel_bytes) != raw_size:
        raise ValueError(f"raw pixel bytes size mismatch: expected {raw_size}, got {len(pixel_bytes)}")

    # compress if requested
    if compression == DM_COMP_NONE:
        data = bytes(pixel_bytes)
    else:
        data = rle_encode(pixel_bytes, bpp)

    data_size = len(data)

    # header sizes
    DM_HEADER_SIZE = 40
    DM_IMG_HDR_SIZE = 12
    raw_header_size = DM_HEADER_SIZE + DM_IMG_HDR_SIZE  # 52
    # Data offset must be 8-byte aligned; header_size field is "Total header size (common + type-specific)"
    padded_header_size = align_up(raw_header_size, 8)   # 56
    data_offset = padded_header_size

    # pack header with checksum=0 for now
    # struct dm_header {
    #   u32 magic;
    #   u32 checksum; // set 0 while computing
    #   u16 version;
    #   u8  type;
    #   u8  compression;
    #   u32 header_size;
    #   u64 data_offset;
    #   u64 data_size;
    #   u64 raw_size;
    # };
    hdr = struct.pack(
        "<I I H B B I Q Q Q",
        DM_MAGIC,
        0,              # placeholder checksum
        DM_VERSION,
        DM_TYPE_IMAGE,
        compression,
        padded_header_size,
        data_offset,
        data_size,
        raw_size
    )

    # image-specific header
    # struct dm_image_header {
    #   u32 width;
    #   u32 height;
    #   u8  pixel_format;
    #   u8  transfer;   // 0 = sRGB
    #   u8  reserved[2]; // zero
    # };
    img_hdr = struct.pack("<I I B B 2B", width, height, pixel_format, 0, 0, 0)

    # pad header to padded_header_size
    header = bytearray()
    header.extend(hdr)
    header.extend(img_hdr)
    pad_len = padded_header_size - len(header)
    if pad_len < 0:
        raise AssertionError("padded header size too small")
    header.extend(b"\x00" * pad_len)

    # compose full file with checksum placeholder
    file_bytes = bytearray()
    file_bytes.extend(header)
    file_bytes.extend(data)

    # compute crc32 with checksum field zeroed (bytes 4..7 are checksum u32 LE)
    # file_bytes[4:8] should already be zero from placeholder pack, but ensure
    file_bytes[4:8] = b"\x00\x00\x00\x00"
    crc = binascii.crc32(file_bytes) & 0xFFFFFFFF

    # write checksum into bytes 4..8 as little-endian u32
    file_bytes[4:8] = struct.pack("<I", crc)

    return bytes(file_bytes), {
        "crc32": crc,
        "header_size": padded_header_size,
        "data_offset": data_offset,
        "data_size": data_size,
        "raw_size": raw_size,
        "bpp": bpp
    }

def main():
    p = argparse.ArgumentParser(description="Convert image -> Delta Media (DM) image")
    p.add_argument("input", help="input image path")
    p.add_argument("-o", "--output", help="output .dm file (default: input.dm)")
    p.add_argument("--format", choices=list(PIX_FORMATS.keys()), default="auto",
                   help="pixel format (default: auto)")
    p.add_argument("--comp", choices=["none","rle"], default="none", help="compression")
    args = p.parse_args()

    in_path = args.input
    out_path = args.output or (in_path.rsplit(".",1)[0] + ".dm")
    target_key = args.format
    compression = DM_COMP_NONE if args.comp == "none" else DM_COMP_RLE

    target_fmt = PIX_FORMATS[target_key]

    # load image
    img = Image.open(in_path)
    width, height, bpp, pixels = image_to_pixel_bytes(img, target_fmt)

    file_bytes, meta = build_dm_image(width, height, target_fmt if target_fmt is not None else (DM_PIXEL_RGBA32 if 'A' in img.mode else DM_PIXEL_RGB24), compression, pixels)

    with open(out_path, "wb") as f:
        f.write(file_bytes)

    print(f"Wrote: {out_path}")
    print(f" width x height: {width} x {height}")
    print(f" pixel_format: {target_key} ({meta['bpp']} BPP)")
    print(f" compression: {'RLE' if compression==DM_COMP_RLE else 'NONE'}")
    print(f" raw_size: {meta['raw_size']} bytes")
    print(f" data_size (stored): {meta['data_size']} bytes")
    print(f" header_size: {meta['header_size']} bytes")
    print(f" data_offset: {meta['data_offset']} bytes")
    print(f" crc32: 0x{meta['crc32']:08x}")

if __name__ == "__main__":
    main()
