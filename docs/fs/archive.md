# Delta Archive Format (DA)

## Overview

DA is a simple archive format for DeltaOS, designed primarily for initramfs.
Optimized for fast sequential parsing during early boot when heap may be limited.

```
┌─────────────────────────────────────────┐
│  DA Header (magic, version, counts)     │
├─────────────────────────────────────────┤
│  Entry Table                            │
│  - Entry 0 (path offset, size, type...) │
│  - Entry 1                              │
│  - ...                                  │
├─────────────────────────────────────────┤
│  String Table (paths, null-terminated)  │
├─────────────────────────────────────────┤
│  File Data (concatenated, aligned)      │
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

## DA Header

```c
struct da_header {
    u32 magic;          // 0x44410001 ('D' 'A' 0x00 0x01)
    u32 checksum;       // CRC32 of header+entry table (with this field as 0)
    u16 version;        // Format version (0x0001)
    u16 flags;          // Archive flags
    u32 entry_count;    // Number of entries
    u32 entry_off;      // Offset to entry table
    u32 strtab_off;     // Offset to string table
    u32 strtab_size;    // Size of string table
    u32 data_off;       // Offset to file data section
    u64 total_size;     // Total uncompressed size of all file data
};
```

**Size:** 40 bytes

### Magic

`0x44410001`:
- `0x44` = 'D'
- `0x41` = 'A'
- `0x00` = separator
- `0x01` = version

### Flags

| Bit | Name              | Description                      |
|-----|-------------------|----------------------------------|
| 0   | `DA_FLAG_SORTED`  | Entries sorted by path (enables binary search) |
| 1   | `DA_FLAG_HASHED`  | Path hashes included for fast lookup |

---

## Entry Table

Each entry describes a file, directory, or symlink.

```c
struct da_entry {
    u32 path_off;       // Offset in string table to path
    u32 flags;          // Entry flags (type, reserved bits)
    u64 data_off;       // Offset from data section start (0 for dirs)
    u64 size;           // Size in bytes (0 for directories)
    u32 hash;           // Path hash (for fast lookup, optional)
    u32 reserved;       // Must be zero
};
```

**Size:** 32 bytes

### Entry Flags

```
Bits 0-3:   Type
Bits 4-31:  Reserved (must be zero, future: permissions)
```

### Entry Types

| Value | Name           | Description          |
|-------|----------------|----------------------|
| 0     | `DA_TYPE_FILE` | Regular file         |
| 1     | `DA_TYPE_DIR`  | Directory            |
| 2     | `DA_TYPE_LINK` | Symbolic link        |

For symlinks, `data_off` points to the target path in the string table.

---

## String Table

Null-terminated paths, stored consecutively. All paths are:
- Absolute (start with `/`)
- Normalized (no `.`, `..`, or trailing slashes)
- UTF-8 encoded

Example:
```
Offset 0:   "/\0"
Offset 2:   "/bin\0"
Offset 7:   "/bin/init\0"
Offset 17:  "/etc\0"
...
```

---

## File Data Section

File contents concatenated sequentially. Each file's data starts at:
```
data_section_start + entry.data_off
```

**Alignment:** Each file's data is aligned to 8 bytes within the data section.
Padding bytes (value 0) are inserted as needed.

---

## Parsing Algorithm

```c
int da_parse(void *archive, size_t size) {
    struct da_header *hdr = archive;
    
    // Verify magic
    if (hdr->magic != 0x44410001) return -1;
    
    // Get tables
    struct da_entry *entries = archive + hdr->entry_off;
    char *strtab = archive + hdr->strtab_off;
    void *data = archive + hdr->data_off;
    
    // Iterate entries
    for (u32 i = 0; i < hdr->entry_count; i++) {
        struct da_entry *e = &entries[i];
        const char *path = strtab + e->path_off;
        u32 type = e->flags & 0xF;
        
        if (type == DA_TYPE_DIR) {
            create_directory(path);
        } else if (type == DA_TYPE_FILE) {
            void *content = data + e->data_off;
            create_file(path, content, e->size);
        } else if (type == DA_TYPE_LINK) {
            const char *target = strtab + e->data_off;
            create_symlink(path, target);
        }
    }
    return 0;
}
```

---

## Path Hashing (Optional)

When `DA_FLAG_HASHED` is set, the `hash` field contains a 32-bit FNV-1a hash
of the path for O(1) average lookup:

```c
u32 da_hash(const char *path) {
    u32 hash = 0x811c9dc5;  // FNV offset basis
    while (*path) {
        hash ^= (u8)*path++;
        hash *= 0x01000193;  // FNV prime
    }
    return hash;
}
```

> [!WARNING]
> Hash is for **filtering**, not matching. Always verify the actual path string
> after a hash match to handle collisions:
> ```c
> if (entry->hash == target_hash && strcmp(path, target) == 0) { found! }
> ```

---

## Comparison to Other Formats

| Feature          | DA      | CPIO    | TAR     | ZIP     |
|------------------|---------|---------|---------|---------|
| Header size      | 40B     | 76-110B | 512B    | 30B+    |
| Random access    | Yes     | No      | No      | Yes     |
| Stream-readable  | Yes*    | Yes     | Yes     | No      |
| Stream-writable  | No      | Yes     | Yes     | No      |
| Path hashing     | Yes     | No      | No      | No      |
| Compression      | No      | No      | No      | Yes     |
| Complexity       | Low     | Medium  | Low     | High    |

*Requires reading entry table into memory first; data section is streamable

---

## Tool: `darc`

The `darc` utility creates and extracts DA archives:

```bash
# Create archive from directory
darc create initramfs.da /path/to/root

# List contents
darc list initramfs.da

# Extract to directory  
darc extract initramfs.da /output/path

# Show info
darc info initramfs.da
```

---

## Security Notes

Loaders MUST validate all offsets before use:

```c
// Bounds checking example
if (e->path_off >= hdr->strtab_size) return -EINVAL;
if (strtab[hdr->strtab_size - 1] != '\0') return -EINVAL;  // ensure null-terminated
if (e->data_off + e->size > archive_size - hdr->data_off) return -EINVAL;
```

> [!CAUTION]
> Never trust offsets from untrusted archives. A malicious archive could point
> `path_off` or `data_off` outside valid bounds, causing out-of-bounds reads.

---

## Notes

- **No compression:** Keep the format simple; compression handled externally (bootloader)
- **No timestamps:** Not needed for initramfs (all files "born" at boot)
- **No UIDs/GIDs:** DeltaOS uses capability-based security, not Unix users
- **Sorted entries:** Enables binary search for path lookups
- **8-byte alignment:** Efficient for 64-bit systems
- **>4GB support:** `data_off` is u64, allowing archives larger than 4GB
- **Tool-created:** DA requires knowing all entries upfront (not stream-writable like tar/cpio).
  Use `darc` tool to create archives; kernel only reads them.
- **Header-only checksum:** CRC32 covers header + entry table only, not file data.
  Bootloader already verifies module integrity; re-checksumming 200MB in early boot is wasteful.

