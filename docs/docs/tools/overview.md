# Tools Overview

The tools directory contains build and utility tools for DeltaOS development and deployment.

## Directory Structure

```
tools/
├── db_patch.py         # Database checksum patching tool
└── darc/               # DeltaArchive creation and manipulation
    ├── darc.c          # Archive tool source
    ├── darc            # Compiled binary
    └── Makefile
```

## Database Patcher (db_patch.py)

### Purpose

Patches DeltaBoot database request headers with correct checksums. Used when modifying boot configuration or database records.

### Features

- Locates DB Request Header by magic number (`0x44420001`)
- Calculates CRC32 checksum over header data
- Supports headers up to 32KB scan size
- Updates file in-place with corrected checksum

### Usage

```bash
python3 tools/db_patch.py <bootdb-file>
```

### Implementation Details

**Magic Number:** `0x44420001` (DB request header identifier)

**Checksum Calculation:**
- Scans file for magic in 8-byte aligned offsets
- Reads header size from offset +10 (2-byte little-endian)
- Temporarily zeros checksum field (offset +4, 4 bytes)
- Computes CRC32 using polynomial `0xEDB88320`
- Writes checksum back to file at offset +4

**Header Structure:**
```
Offset  Size  Field
0       4     Magic (0x44420001)
4       4     Checksum (calculated)
8       2     Version
10      2     Header size
...     ...   Additional header data
```

### Error Handling

- Prints error if magic not found in first 32KB
- Verifies header size validity
- Reports calculated checksum for verification

## DeltaArchive Tool (darc)

### Purpose

Creates, reads, and manipulates DeltaArchive (DA) format files - a binary archive format used for read-only filesystems on DeltaOS.

### Features

- **Archive Creation:** Pack files into DA format
- **Archive Reading:** Extract and list archive contents
- **Sorting:** Optional directory sorting for efficient lookup
- **Hashing:** CRC32 checksums for integrity
- **String Table:** Efficient string deduplication
- **Compression:** Optional compression support

### Usage

```bash
make -C tools/darc
./tools/darc/darc [options] <archive> [files...]
```

### Archive Format

**File Extension:** `.da`

**Magic Number:** `0x44410001` (DA magic identifier)

**Header Structure (da_header_t):**

| Offset | Size | Field | Purpose |
|--------|------|-------|---------|
| 0      | 4    | magic | File identifier (0x44410001) |
| 4      | 4    | checksum | CRC32 of header |
| 8      | 2    | version | Archive format version |
| 10     | 2    | flags | Archive flags (sorted, hashed) |
| 12     | 4    | entry_count | Number of entries |
| 16     | 4    | entry_off | Offset to entry table |
| 20     | 4    | strtab_off | Offset to string table |
| 24     | 4    | strtab_size | Size of string table |
| 28     | 4    | data_off | Offset to file data |
| 32     | 8    | total_size | Total archive size |

**Archive Flags:**

- `DA_FLAG_SORTED (1 << 0)` - Entries are sorted (binary search capable)
- `DA_FLAG_HASHED (1 << 1)` - Entries have hash values

**Entry Structure (da_entry_t):**

| Offset | Size | Field | Purpose |
|--------|------|-------|---------|
| 0      | 4    | path_off | Offset into string table |
| 4      | 4    | flags | Entry flags (type field) |
| 8      | 8    | data_off | Offset to file data in archive |
| 16     | 8    | size | File size in bytes |
| 24     | 4    | hash | CRC32 hash of entry |
| 28     | 4    | reserved | Reserved for future use |

**Entry Types:**

- `DA_TYPE_FILE (0)` - Regular file
- `DA_TYPE_DIR (1)` - Directory
- `DA_TYPE_LINK (2)` - Symbolic/hard link

**String Table:**
- Concatenated null-terminated strings
- Entry path_off values index into this table
- Deduplicated for efficiency

### Typical Workflow

1. **Create archive from files:**
   ```bash
   ./darc myfiles.da file1 file2 dir/file3
   ```

2. **List archive contents:**
   ```bash
   ./darc -l myfiles.da
   ```

3. **Extract archive:**
   ```bash
   ./darc -x myfiles.da /output/path
   ```

### Integration with DeltaOS

DeltaArchive format is used by:

- **Initial Ramdisk (initrd):** System files packed as DA archive
- **Read-only Filesystems:** Efficient boot-time filesystem
- **System Images:** Distribution packages

### Implementation Notes

- Written in standard C99
- Portable to Unix/Linux systems
- Single-threaded processing
- Deterministic output for reproducible builds

## Building Tools

### Build All Tools

```bash
make -C tools/darc
```

### Individual Tool Builds

All tools are built in-place:

- `db_patch.py` - No build needed (Python script)
- `darc` - Built by `make -C tools/darc`

### Output

- `tools/darc/darc` - Compiled archive tool executable

## Tool Dependencies

### db_patch.py

**Runtime Dependencies:**
- Python 3.x
- `zlib` module (standard library)
- `struct` module (standard library)

**No external requirements**

### darc

**Build Dependencies:**
- C99-compliant compiler (GCC, Clang)
- Standard POSIX environment
- Standard C library (libc)

**No external libraries required - uses only POSIX/libc**

## Development Usage

### When to Use db_patch.py

- After modifying boot database records
- When database headers are corrupted
- During boot configuration changes
- Before flashing to storage media

### When to Use darc

- Packing system files for distribution
- Creating custom root filesystems
- Building embedded images
- Verifying archive integrity

## File Locations in Build

| File | Location | Purpose |
|------|----------|---------|
| db_patch.py | `tools/db_patch.py` | Runtime tool |
| darc | `tools/darc/darc` | Runtime tool |
| darc.c | `tools/darc/darc.c` | Source code |
| Makefile | `tools/darc/Makefile` | Build script |

## Maintenance

### Adding New Tools

1. Create subdirectory under `tools/`
2. Add Makefile and source files
3. Ensure standalone compilation
4. Document usage and format

### Tool Compatibility

- All tools are standalone (no deltaos kernel required)
- Portable to development host systems (Linux, macOS, Windows/WSL)
- No dependencies on kernel or user space
