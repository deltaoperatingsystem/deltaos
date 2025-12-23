#!/usr/bin/env python3
import sys
import struct
import zlib

DB_REQUEST_MAGIC = 0x44420001
SCAN_SIZE = 32 * 1024

def patch_checksum(filename):
    with open(filename, "rb+") as f:
        data = f.read(SCAN_SIZE)
        
        #find magic
        offset = -1
        for i in range(0, len(data) - 4, 8):
            magic = struct.unpack_from("<I", data, i)[0]
            if magic == DB_REQUEST_MAGIC:
                offset = i
                break
        
        if offset == -1:
            print(f"Error: DB Request Header not found in {filename}")
            sys.exit(1)
            
        print(f"Found DB Request Header at offset {offset}")
        
        #read header size
        #magic(4) + checksum(4) + version(2) + header_size(2)
        header_size = struct.unpack_from("<H", data, offset + 10)[0]
        print(f"Header size: {header_size}")
        
        #read the whole header
        header = bytearray(data[offset : offset + header_size])
        
        #set checksum to 0 for calculation
        header[4:8] = b"\x00\x00\x00\x00"
        
        #calculate CRC32 (DB uses standard CRC32 polynomial 0xEDB88320)
        #zlib.crc32(data) & 0xffffffff does exactly this
        checksum = zlib.crc32(header) & 0xffffffff
        
        print(f"Calculated checksum: 0x{checksum:08X}")
        
        #write checksum back to file
        f.seek(offset + 4)
        f.write(struct.pack("<I", checksum))
        print("Successfully patched checksum.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: db_patch.py <elf_file>")
        sys.exit(1)
    patch_checksum(sys.argv[1])
