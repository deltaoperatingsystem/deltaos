# Delta Executable Format (DX)

## Overview

DX is the native executable format for DeltaOS 
simpler than ELF/PE/Mach-O while still supporting:

- Position-independent code
- Dynamic linking
- Multiple architectures

```
┌─────────────────────────────────────────┐
│  DX Header (magic, version, arch)       │
├─────────────────────────────────────────┤
│  Segment Table                          │
│  - Code segment                         │
│  - Data segment                         │
│  - ...                                  │
├─────────────────────────────────────────┤
│  Symbol Table (optional)                │
├─────────────────────────────────────────┤
│  Relocation Table (optional)            │
├─────────────────────────────────────────┤
│  Raw segment data                       │
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

## DX Header

The header is split into a common part (architecture-independent) followed by
an architecture-specific part. The `header_size` field indicates total size.

### Common Header

```c
struct dx_header {
    u32 magic;          // 0x44580001 ('D' 'X' 0x00 0x01)
    u32 checksum;       // CRC32 of file (with this field as 0)
    u16 version;        // Format version (0x0001)
    u16 type;           // Executable type (see below)
    u16 arch;           // Target architecture
    u16 flags;          // Executable flags
    u16 header_size;    // Total header size (common + arch)
    u16 reserved;       // Must be zero (reject if non-zero)
    u32 segment_off;    // Offset to segment table
    u16 segment_count;  // Number of segments
    u16 segment_size;   // Size of each segment entry
    u32 symbol_off;     // Offset to symbol table (0 = none)
    u32 symbol_count;   // Number of symbols
    u32 strtab_off;     // Offset to string table
    u32 strtab_size;    // Size of string table in bytes
    u32 reloc_off;      // Offset to relocation table (0 = none)
    u32 reloc_count;    // Number of relocations
    u32 prelink_off;    // Offset to prelink cache (0 = none)
    // Architecture-specific part follows immediately
};
```

**Common size:** 56 bytes

> **Reserved fields:** All `reserved` fields MUST be zero. Loaders MUST reject
> files with non-zero reserved fields. This allows clean version bumps.

### Architecture-Specific Header

> **Rule:** The arch-specific part MUST be a strict extension of the common header.
> No reordering, no conditional fields, no shared offsets. This ensures cross-arch
> tools can parse the common header without knowing the architecture.

#### AMD64 (so 64-bit)

```c
struct dx_header_amd64 {
    u64 entry;          // Entry point (virtual address, or offset if PIE)
};
```

**Total header size (AMD64):** 64 bytes (56 + 8)

#### x86 (so 32-bit)

```c
struct dx_header_x86 {
    u32 entry;          // Entry point
};
```

**Total header size (x86):** 60 bytes (56 + 4)

> **Note:** For PIE executables, add the load base to entry to get actual address.

### Magic

`0x44580001`:
- `0x44` = 'D'
- `0x58` = 'X'
- `0x00` = separator
- `0x01` = version

### Type

| Value | Name          | Description                    |
|-------|---------------|--------------------------------|
| 0     | `DX_EXEC`     | Executable                     |
| 1     | `DX_DYN`      | Dynamic library (shared object)|
| 2     | `DX_OBJ`      | Relocatable object file        |

### Architecture

| Value | Name            | Arch Header Size | Description         |
|-------|-----------------|------------------|---------------------|
| 0     | `DX_ARCH_ANY`   | 0                | Architecture-independent |
| 1     | `DX_ARCH_AMD64` | 8                | x86-64              |
| 2     | `DX_ARCH_X86`   | 4                | x86 (32-bit)        |
| 3     | `DX_ARCH_ARM64` | 8                | AArch64             |
| 4     | `DX_ARCH_ARM32` | 4                | ARM (32-bit)        |
| 5     | `DX_ARCH_RISCV64` | 8              | RISC-V 64-bit       |

### Flags

| Bit | Name              | Description                      |
|-----|-------------------|----------------------------------|
| 0   | `DX_FLAG_PIE`     | Position-independent executable  |
| 1   | `DX_FLAG_STATIC`  | Statically linked                |
| 2   | `DX_FLAG_DEBUG`   | Contains debug information       |
| 3   | `DX_FLAG_LAZY`    | Lazy symbol resolution (resolve on first call) |

---

## Segment Table

Each segment describes a contiguous region to load.

```c
struct dx_segment {
    u32 type;           // Segment type
    u32 flags;          // Permissions (RWX)
    u64 file_off;       // Offset in file
    u64 file_size;      // Size in file (may be 0 for BSS)
    u64 mem_addr;       // Virtual address (or offset if PIE)
    u64 mem_size;       // Size in memory (>= file_size for BSS)
    u64 align;          // Alignment requirement
};
```

**Size:** 48 bytes

### Segment Types

| Value | Name           | Description              |
|-------|----------------|--------------------------|
| 0     | `DX_SEG_NULL`  | Unused entry             |
| 1     | `DX_SEG_LOAD`  | Loadable segment         |
| 2     | `DX_SEG_DYN`   | Dynamic linking info     |
| 3     | `DX_SEG_NOTE`  | Note/comment             |

### Segment Flags (Permissions)

| Bit | Name  | Description |
|-----|-------|-------------|
| 0   | `R`   | Readable    |
| 1   | `W`   | Writable    |
| 2   | `X`   | Executable  |

---

## Symbol Table

For dynamic linking and debugging.

```c
struct dx_symbol {
    u32 name_off;       // Offset to name string (in string table)
    u16 type;           // Symbol type
    u16 bind;           // Binding (local/global/weak)
    u64 value;          // Value (address or offset)
    u64 size;           // Size of symbol
    u16 segment;        // Segment index (0xFFFF = absolute)
    u16 reserved;
};
```

**Size:** 28 bytes

### Symbol Types

| Value | Name             | Description         |
|-------|------------------|---------------------|
| 0     | `DX_SYM_NONE`    | No type             |
| 1     | `DX_SYM_FUNC`    | Function            |
| 2     | `DX_SYM_DATA`    | Data object         |
| 3     | `DX_SYM_SECTION` | Section             |

### Symbol Binding

| Value | Name             | Description              |
|-------|------------------|--------------------------|
| 0     | `DX_BIND_LOCAL`  | Not visible outside file |
| 1     | `DX_BIND_GLOBAL` | Visible, can be resolved |
| 2     | `DX_BIND_WEAK`   | Weak symbol              |

---

## Relocation Table

For position-independent code.

```c
struct dx_reloc {
    u64 offset;         // Offset to apply relocation
    u16 type;           // Relocation type (arch-specific)
    u16 segment;        // Segment containing the offset
    u32 symbol;         // Symbol index (or 0 for relative)
    i64 addend;         // Addend for calculation
};
```

**Size:** 24 bytes

### Relocation Types (AMD64)

| Value | Name                 | Calculation           |
|-------|----------------------|-----------------------|
| 0     | `DX_R_NONE`          | None                  |
| 1     | `DX_R_64`            | S + A                 |
| 2     | `DX_R_PC32`          | S + A - P             |
| 3     | `DX_R_PLT32`         | L + A - P             |
| 4     | `DX_R_RELATIVE`      | B + A                 |

Where: S = symbol value, A = addend, P = place, B = base address and L = PLT entry

---

## String Table

Zero-terminated strings, referenced by offset.

---

## Loading Algorithm

```c
int dx_load(void *file, size_t size, uintptr load_base) {
    struct dx_header *hdr = file;
    
    // Verify magic and version
    if (hdr->magic != 0x44580001) return -1;
    if (hdr->arch != DX_ARCH_AMD64) return -1;
    
    // Load segments
    struct dx_segment *segs = file + hdr->segment_off;
    for (int i = 0; i < hdr->segment_count; i++) {
        if (segs[i].type != DX_SEG_LOAD) continue;
        
        uintptr dest = segs[i].mem_addr;
        if (hdr->flags & DX_FLAG_PIE) dest += load_base;
        
        mmap((void *)dest, segs[i].mem_size, segs[i].flags);
        memcpy((void *)dest, file + segs[i].file_off, segs[i].file_size);
        memset((void *)(dest + segs[i].file_size), 0, 
               segs[i].mem_size - segs[i].file_size);
    }
    
    // Apply relocations (if PIE)
    if (hdr->flags & DX_FLAG_PIE) {
        apply_relocations(file, hdr, load_base);
    }
    
    // Jump to entry point (add base for PIE)
    uintptr entry_addr = hdr->entry;
    if (hdr->flags & DX_FLAG_PIE) entry_addr += load_base;
    
    void (*entry)(void) = (void *)entry_addr;
    entry();
}
```

---

## Comparison to Other Formats

| Feature          | DX    | ELF     | PE      | Mach-O  |
|------------------|-------|---------|---------|---------|
| Header size      | 56-64B| 64B+    | 248B+   | 32B+    |
| Fixed tables     | Yes   | No      | No      | No      |
| Section names    | No    | Yes     | Yes     | Yes     |
| Segment/Section  | Merged| Separate| Separate| Separate|
| Prelink cache    | Yes   | No      | No      | No      |
| Lazy binding     | Yes   | Yes     | Yes     | Yes     |
| Checksum         | CRC32 | Optional| Yes     | Optional|
| Complexity       | Low   | Medium  | High    | High    |

---

## Notes

- **ELF compatibility:** DeltaOS will support ELF during transition for toolchain compatibility
- **Future extensions:** Types 0x8000+ reserved for extensions

---

## Checksum

The `checksum` field contains a **CRC32** (polynomial `0xEDB88320`) of the entire file,
with the `checksum` field itself treated as zero during computation.

**Verification is optional** — loaders may skip for speed when loading from trusted sources.
System libraries can skip verification; untrusted executables should verify.

---

## Prelink Cache

The prelink cache stores pre-resolved symbol addresses for faster loading.
When `prelink_off` is non-zero, it points to:

```c
struct dx_prelink {
    u32 count;              // Number of cached entries
    u32 base_valid;         // 1 if cache matches current library layout
    u64 base_addr;          // Base address cache was computed for
    struct dx_prelink_entry entries[];
};

struct dx_prelink_entry {
    u32 symbol;             // Symbol index
    u32 reserved;           // Padding for alignment
    u64 resolved_addr;      // Pre-resolved address
};
```

**Usage:**
- If `base_valid` and libraries haven't changed, use cached addresses directly
- Otherwise, fall back to normal symbol resolution
- Tools like `dx-prelink` regenerate cache when system libraries update

---

## Lazy Binding

When `DX_FLAG_LAZY` is set, external symbols are resolved on first call:

1. PLT entries initially point to resolver stub
2. First call invokes resolver, patches PLT with real address
3. Subsequent calls go directly to resolved address

This speeds up startup by deferring work, until actually needed.