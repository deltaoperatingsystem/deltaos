# Delta Boot Protocol

## Overview

The DB protocol defines:

1. **DB Request Header** — Embedded in the kernel binary, tells the bootloader what's needed
2. **DB Boot Info** — Passed to the kernel at boot, contains system information as tags

```
┌──────────────────────────────────────────────────────────────┐
│                    KERNEL BINARY                             │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  DB Request Header (magic + version + flags)           │  │
│  │  Request Tags: what the kernel wants from bootloader   │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
                           │
                           ▼ bootloader reads & prepares
┌──────────────────────────────────────────────────────────────┐
│                    DB BOOT INFO                              │
│  Passed to kernel entry point (pointer in arch-defined reg)  │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  Info Header                                           │  │
│  ├────────────────────────────────────────────────────────┤  │
│  │  Tag: Memory Map                                       │  │
│  │  Tag: Framebuffer                                      │  │
│  │  Tag: Command Line                                     │  │
│  │  Tag: ...                                              │  │
│  │  Tag: End                                              │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

---

## Data Types

All multi-byte integers are **little-endian**=

| Type   | Size    | Description          |
|--------|---------|----------------------|
| `u8`   | 1 byte  | Unsigned 8-bit       |
| `u16`  | 2 bytes | Unsigned 16-bit LE   |
| `u32`  | 4 bytes | Unsigned 32-bit LE   |
| `u64`  | 8 bytes | Unsigned 64-bit LE   |

---

## Part 1: DB Request Header

The kernel embeds this header to identify itself as DB-compliant and request features.

### Location

The bootloader scans the first **32 KiB** of the kernel binary for the magic number,
Header must be **8-byte aligned**.

### Structure

```c
struct db_request_header {
    u32 magic;          // 0x44420001 ('D' 'B' 0x00 0x01)
    u32 checksum;       // CRC32 of header + tags (with this field as 0)
    u16 version;        // Protocol version (0x0001 for v0.1)
    u16 header_size;    // Size of this header + all request tags
    u32 flags;          // Request flags (see below)
    u32 entry_point;    // Offset (not address!) of kernel entry point
                        // Relative to start of loaded kernel image
                        // Use 0xFFFFFFFF if bootloader should use format-specific entry
};
// Followed by request tags (until header_size is reached)
```

**Magic breakdown:** `0x44420001`
- `0x44` = 'D'
- `0x42` = 'B'  
- `0x00` = null separator
- `0x01` = format version

### Checksum

The `checksum` field contains a **CRC32** (polynomial `0xEDB88320`)
computed over the entire header including request tags, with the `checksum` field itself
treated as zero during computation.

**To compute:**
1. Set `checksum` to 0
2. Calculate CRC32 over bytes `[0, header_size)`
3. Store result in `checksum`

**To verify:**
1. Save `checksum` value
2. Set `checksum` to 0
3. Calculate CRC32 over bytes `[0, header_size)`
4. Compare with saved value

### Request Flags

| Bit | Name                  | Description                                      |
|-----|-----------------------|--------------------------------------------------|
| 0   | `DB_REQ_FRAMEBUFFER`  | Kernel requests framebuffer info                 |
| 1   | `DB_REQ_MEMORY_MAP`   | Kernel requests memory map (should always be set)|
| 2   | `DB_REQ_MODULES`      | Kernel supports loaded modules                   |
| 3   | `DB_REQ_ACPI`         | Kernel requests ACPI RSDP pointer                |
| 4   | `DB_REQ_CMDLINE`      | Kernel accepts command line                      |
| 5   | `DB_REQ_SMP`          | Kernel requests SMP info (CPU count, etc.)       |
| 6   | `DB_REQ_INITRD`       | Kernel requests initial ramdisk                  |
| 7   | `DB_REQ_HAS_TAGS`     | Request tags follow the header                   |
| 8-31| Reserved              | Must be zero                                     |

### Flags and Tags Precedence

- **Flag set, no tag:** Bootloader provides the feature with default values
- **Flag set, tag present:** Bootloader uses tag values (tag refines the request)
- **Flag not set, tag present:** Tag is ignored (flag must be set to enable feature)

---

## Part 1b: Request Tags

Request tags allow the kernel to specify detailed requirements beyond simple flags.
They immediately follow the request header and are included in `header_size`.

### Request Tag Structure

```c
struct db_request_tag {
    u16 type;           // Request tag type
    u16 flags;          // Tag-specific flags
    u32 size;           // Total size of this tag (including header)
    // Tag-specific data follows
};
```

**Alignment:** Each request tag starts at a 4-byte aligned offset from header start

### Request Tag Types

| Type   | Name                       | Description                           |
|--------|----------------------------|---------------------------------------|
| 0x0000 | `DB_RTAG_END`              | End of request tag list               |
| 0x0001 | `DB_RTAG_FRAMEBUFFER_PREF` | Preferred framebuffer settings        |
| 0x0002 | `DB_RTAG_MIN_MEMORY`       | Minimum memory requirement            |
| 0x0003 | `DB_RTAG_LOAD_ADDRESS`     | Preferred/required load address       |
| 0x0004 | `DB_RTAG_STACK_SIZE`       | Requested initial stack size          |
| 0x0005 | `DB_RTAG_ARCH_FEATURES`    | Architecture-specific feature requests|

---

### DB_RTAG_FRAMEBUFFER_PREF (0x0001)

Specify preferred framebuffer resolution and pixel format.

```c
struct db_rtag_framebuffer_pref {
    u16 type;           // 0x0001
    u16 flags;          // Bit 0: required (fail if unavailable)
    u32 size;           // 24
    u32 min_width;      // Minimum acceptable width (0 = any)
    u32 min_height;     // Minimum acceptable height (0 = any)
    u32 preferred_width;  // Preferred width (0 = any)
    u32 preferred_height; // Preferred height (0 = any)
    u8  min_bpp;        // Minimum bits per pixel (0 = any)
    u8  preferred_bpp;  // Preferred bpp (0 = any)
    u8  padding[2];
};
```

---

### DB_RTAG_MIN_MEMORY (0x0002)

Specify minimum RAM requirement.

```c
struct db_rtag_min_memory {
    u16 type;           // 0x0002
    u16 flags;          // 0
    u32 size;           // 16
    u64 min_bytes;      // Minimum usable RAM required
};
```

---

### DB_RTAG_LOAD_ADDRESS (0x0003)

Request a specific load address (hint or requirement).

```c
struct db_rtag_load_address {
    u16 type;           // 0x0003
    u16 flags;          // Bit 0: required (fail if unavailable)
    u32 size;           // 24
    u64 preferred_addr; // Preferred physical load address
    u64 alignment;      // Required alignment (must be power of 2)
};
```

---

### DB_RTAG_STACK_SIZE (0x0004)

Request initial kernel stack size.

```c
struct db_rtag_stack_size {
    u16 type;           // 0x0004
    u16 flags;          // 0
    u32 size;           // 16
    u64 stack_size;     // Requested stack size in bytes (0 = bootloader default)
};
```

---

## Part 2: DB Boot Info

Prepared by the bootloader and passed to the kernel.

### Passing Convention

The bootloader passes a pointer to `db_boot_info` in an **architecture-defined register**:

| Architecture | Register | Notes                          |
|--------------|----------|--------------------------------|
| x86 (32-bit) | `EBX`    | Physical address               |
| x86_64       | `RDI`    | Physical address (first arg)   |
| ARM32        | `R0`     | Physical address               |
| AArch64      | `X0`     | Physical address               |
| RISC-V       | `A0`     | Physical address               |

### Info Header Structure

```c
struct db_boot_info {
    u32 magic;          // 0x44424F4B ('D' 'B' 'O' 'K' — "DB OK")
    u32 total_size;     // Total size of boot info including all tags
    u32 version;        // Protocol version
    u32 reserved;       // Must be zero
};
// Immediately followed by tags
```

---

## Part 3: Tags

Tags are variable-length structures that follow the header.

### Tag Structure

```c
struct db_tag {
    u16 type;           // Tag type identifier
    u16 flags;          // Tag-specific flags
    u32 size;           // Total size of this tag (including header)
    // Tag-specific data follows
};
```

**Alignment:** Each tag starts at an 8-byte aligned address.  
**Padding:** Bootloader inserts padding bytes (value 0) between tags as needed.

### Tag Types

| Type   | Name                    | Description                        |
|--------|-------------------------|------------------------------------|
| 0x0000 | `DB_TAG_END`            | End of tag list                    |
| 0x0001 | `DB_TAG_CMDLINE`        | Command line string                |
| 0x0002 | `DB_TAG_MEMORY_MAP`     | System memory map                  |
| 0x0003 | `DB_TAG_FRAMEBUFFER`    | Framebuffer information            |
| 0x0004 | `DB_TAG_MODULES`        | Loaded modules (generic)           |
| 0x0005 | `DB_TAG_ACPI_RSDP`      | ACPI RSDP pointer                  |
| 0x0006 | `DB_TAG_SMP`            | SMP/CPU information                |
| 0x0007 | `DB_TAG_BOOT_TIME`      | Boot timestamp                     |
| 0x0008 | `DB_TAG_BOOTLOADER`     | Bootloader name/version            |
| 0x0009 | `DB_TAG_KERNEL_FILE`    | Original kernel file info          |
| 0x000A | `DB_TAG_EFI_SYSTEM_TABLE` | EFI System Table pointer (if UEFI) |
| 0x000B | `DB_TAG_INITRD`         | Initial ramdisk (initrd/initramfs) |
| 0x8000+ | Vendor-specific        | Reserved for custom extensions     |

---

## Tag Definitions

### DB_TAG_END (0x0000)

Marks the end of the tag list. Required.

```c
struct db_tag_end {
    u16 type;           // 0x0000
    u16 flags;          // 0
    u32 size;           // 8
};
```

---

### DB_TAG_CMDLINE (0x0001)

Null-terminated command line string.

```c
struct db_tag_cmdline {
    u16 type;           // 0x0001
    u16 flags;          // 0
    u32 size;           // 8 + string length + 1 (null terminator)
    char cmdline[];     // Null-terminated UTF-8 string
};
```

---

### DB_TAG_MEMORY_MAP (0x0002)

Describes physical memory layout.

```c
struct db_tag_memory_map {
    u16 type;           // 0x0002
    u16 flags;          // 0
    u32 size;           // Total tag size
    u32 entry_size;     // Size of each entry (for forward compat)
    u32 entry_count;    // Number of entries
    struct db_mmap_entry entries[];
};

struct db_mmap_entry {
    u64 base;           // Physical base address
    u64 length;         // Length in bytes
    u32 type;           // Memory type (see below)
    u32 attributes;     // Additional attributes
};
```

**Memory Types:**

| Value | Name                    | Description                        |
|-------|-------------------------|------------------------------------|
| 0     | `DB_MEM_RESERVED`       | Reserved, do not use               |
| 1     | `DB_MEM_USABLE`         | Free RAM, available for use        |
| 2     | `DB_MEM_ACPI_RECLAIMABLE` | ACPI tables, reclaimable         |
| 3     | `DB_MEM_ACPI_NVS`       | ACPI Non-Volatile Storage          |
| 4     | `DB_MEM_BAD`            | Bad memory, do not use             |
| 5     | `DB_MEM_BOOTLOADER`     | Used by bootloader, reclaimable    |
| 6     | `DB_MEM_KERNEL`         | Kernel image                       |
| 7     | `DB_MEM_FRAMEBUFFER`    | Framebuffer memory                 |
| 8     | `DB_MEM_INITRD`         | Initial ramdisk, reclaimable       |
| 9     | `DB_MEM_MODULES`        | Loaded modules                     |

---

### DB_TAG_FRAMEBUFFER (0x0003)

Framebuffer information for graphical output.

```c
struct db_tag_framebuffer {
    u16 type;           // 0x0003
    u16 flags;          // 0
    u32 size;           // Tag size
    u64 address;        // Physical address of framebuffer
    u32 width;          // Width in pixels
    u32 height;         // Height in pixels
    u32 pitch;          // Bytes per scanline
    u8  bpp;            // Bits per pixel
    u8  red_shift;      // Red component bit position
    u8  red_size;       // Red component bit size
    u8  green_shift;
    u8  green_size;
    u8  blue_shift;
    u8  blue_size;
    u8  reserved_shift;
    u8  reserved_size;
    u8  padding[3];     // Alignment padding
};
```

---

### DB_TAG_MODULES (0x0004)

Loaded boot modules (initramfs, drivers, etc.).

```c
struct db_tag_modules {
    u16 type;           // 0x0004
    u16 flags;          // 0
    u32 size;           // Total tag size
    u32 module_count;   // Number of modules
    u32 reserved;
    struct db_module modules[];
};

struct db_module {
    u64 start;          // Physical start address
    u64 end;            // Physical end address (exclusive)
    u32 name_offset;    // Offset to null-terminated name string (from tag start)
    u32 cmdline_offset; // Offset to null-terminated cmdline (from tag start)
};
// Module names and cmdlines stored as strings after the module array
```

---

### DB_TAG_INITRD (0x000B)

Initial ramdisk (initrd or initramfs) 
This is the primary way to load a initial
filesystem for early boot.

```c
struct db_tag_initrd {
    u16 type;           // 0x000B
    u16 flags;          // 0
    u32 size;           // Tag size
    u64 start;          // Physical start address of initrd
    u64 length;         // Size in bytes
};
```

**Notes:**
- The bootloader loads the initrd file into contiguous physical memory
- The kernel is responsible for parsing the format (cpio, ext2, etc.)
- Memory region is marked as `DB_MEM_INITRD` in the memory map (reclaimable after use)

---

### DB_TAG_ACPI_RSDP (0x0005)

ACPI Root System Description Pointer.

```c
struct db_tag_acpi_rsdp {
    u16 type;           // 0x0005
    u16 flags;          // Bit 0: set if XSDP (ACPI 2.0+)
    u32 size;           // Tag size
    u64 rsdp_address;   // Physical address of RSDP/XSDP
};
```

---

### DB_TAG_SMP (0x0006)

Symmetric Multi Processing information.

```c
struct db_tag_smp {
    u16 type;           // 0x0006
    u16 flags;          // 0
    u32 size;           // Tag size
    u32 cpu_count;      // Number of CPUs
    u32 bsp_id;         // Bootstrap processor ID
    struct db_cpu cpus[];
};

struct db_cpu {
    u32 id;             // CPU/APIC ID (arch-specific)
    u32 flags;          // Bit 0: enabled, Bit 1: is BSP
};
```

---

### DB_TAG_BOOTLOADER (0x0008)

Bootloader identification.

```c
struct db_tag_bootloader {
    u16 type;           // 0x0008
    u16 flags;          // 0
    u32 size;           // Tag size
    char name[];        // Null-terminated bootloader name/version
};
```

---

## Minimal Kernel Example (Pseudocode)

```c
// Entry point receives db_boot_info pointer
void kernel_main(struct db_boot_info* info) {
    // Verify magic
    if (info->magic != 0x44424F4B) {
        panic("Not booted via DB protocol");
    }
    
    // Iterate tags
    struct db_tag* tag = (void*)info + sizeof(*info);
    
    while (tag->type != DB_TAG_END) {
        switch (tag->type) {
            case DB_TAG_MEMORY_MAP:
                parse_memory_map((struct db_tag_memory_map*)tag);
                break;
            case DB_TAG_FRAMEBUFFER:
                init_framebuffer((struct db_tag_framebuffer*)tag);
                break;
            //... handle other tags
        }
        
        // Advance to next tag (8-byte aligned)
        tag = (void*)tag + ((tag->size + 7) & ~7);
    }
}
```

---

## Bootloader Requirements

A DB-compliant bootloader must:

1. **Locate the DB Request Header** in the kernel binary (first 32 KiB, 8-byte aligned)
2. **Verify the magic** (`0x44420001`)
3. **Load the kernel** into memory at an appropriate address (bootloader's choice)
4. **Prepare DB Boot Info** with requested tags based on request flags
5. **Set up machine state:**
   - Interrupts disabled
   - Paging **enabled** with identity mapping (virtual = physical)
   - A20 line enabled (if x86)
6. **Pass boot info pointer** in the architecture-defined register
7. **Jump to kernel entry point**

### Load Address

The bootloader chooses where to load the kernel in physical memory. The kernel must be
position-independent or linked to expect this. The kernel can determine its load address
from the memory map (look for `DB_MEM_KERNEL` regions).
