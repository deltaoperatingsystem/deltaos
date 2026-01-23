# Kernel Subsystems Reference

## Architecture (`arch/`)

Provides abstraction layer for AMD64-specific functionality.

### Files

| File | Purpose |
|------|---------|
| `cpu.h` | CPU capabilities and identification |
| `context.h/context.c` | Thread register state and context switching |
| `interrupts.h` | Interrupt setup and handlers |
| `io.h` | Port I/O operations (in/out instructions) |
| `mmu.h` | Memory Management Unit (page tables, TLB) |
| `timer.h` | Timer tick and interrupt handling |
| `types.h` | Architecture-specific type definitions |
| `amd64/` | AMD64-specific implementations |

## Memory Management (`mm/`)

Manages physical and virtual memory allocation.

### Files

| File | Purpose |
|------|---------|
| `pmm.h/pmm.c` | Physical Memory Manager - allocates/frees physical pages |
| `vmm.h/vmm.c` | Virtual Memory Manager - manages page tables and mappings |
| `kheap.h/kheap.c` | Kernel heap allocator for dynamic allocations |
| `vmo.h/vmo.c` | Virtual Memory Objects - shareable memory regions |
| `mm.h` | Macro definitions (P2V, V2P for HHDM conversions) |

### Key Concepts

- **PMM** allocates physical pages from available RAM
- **VMM** manages virtual address spaces and page tables
- **Kheap** provides malloc/free for kernel code
- **VMO** objects support copy-on-write and sharing between processes
- **HHDM** (Higher Half Direct Map) maps all physical RAM at a fixed virtual offset

## Process Management (`proc/`)

Manages processes, threads, and scheduling.

### Files

| File | Purpose |
|------|---------|
| `process.h/process.c` | Process structure with handle tables and memory |
| `thread.h/thread.c` | Thread execution contexts and state |
| `sched.h/sched.c` | Process/thread scheduler |
| `wait.h/wait.c` | Synchronization primitives (wait, notify) |

### Process Structure

```
Process
├── PID (process ID)
├── Name and CWD (current working directory)
├── State (READY, RUNNING, BLOCKED, DEAD, ZOMBIE)
├── Handle Table (capability table)
├── Virtual Memory Areas (VMAs)
└── Threads
```

### Process States

- **READY** - Runnable, waiting for CPU
- **RUNNING** - Currently executing
- **BLOCKED** - Waiting for I/O or synchronization
- **DEAD** - Terminated, cleaned up
- **ZOMBIE** - Terminated, waiting for parent wait()

## Filesystem (`fs/`)

Provides virtual filesystem with multiple backends.

### Files

| File | Purpose |
|------|---------|
| `fs.h` | Filesystem interface and operations |
| `da.h/da.c` | DeltaArchive format handler (read-only) |
| `initrd.h/initrd.c` | Initial ramdisk with compressed filesystem |
| `tmpfs.h/tmpfs.c` | In-memory temporary filesystem |

### Filesystem Operations

- `lookup(path)` - Find object at path
- `create(path, type)` - Create new file/dir
- `remove(path)` - Delete file/dir
- `readdir(path)` - List directory entries
- `stat(path)` - Get file status

### Node Types

- **FILE** - Regular file
- **DIR** - Directory
- **SYMLINK** - Symbolic link
- **LINK** - Hard link
- **PIPE** - Named pipe (FIFO)
- **SOCKET** - Socket
- **DEVICE** - Device node

## Drivers (`drivers/`)

Hardware abstraction and device drivers.

### Files

| File | Purpose |
|------|---------|
| `console.h/console.c` | Text console output (VGA/framebuffer) |
| `fb.h/fb.c` | Framebuffer graphics driver |
| `keyboard.h/keyboard.c` | Keyboard input handling |
| `keyboard_protocol.h` | Keyboard protocol definitions |
| `mouse.h/mouse.c` | Mouse input handling |
| `mouse_protocol.h` | Mouse protocol definitions |
| `pci.h/pci.c` | PCI bus enumeration and device management |
| `pci_protocol.h` | PCI protocol definitions |
| `rtc.h/rtc.c` | Real-time clock driver |
| `serial.h/serial.c` | Serial port driver (COM ports) |
| `vt/` | Virtual terminal implementation |

## IPC (`ipc/`)

Inter-process communication via message-passing channels.

### Files

| File | Purpose |
|------|---------|
| `channel.h/channel.c` | Bidirectional message-passing endpoints |
| `channel_server.h` | Server-side channel management |

### Channel IPC Model

- Two-endpoint channels for bidirectional communication
- Message-based with copy semantics
- Supports handle passing between processes

## Kernel Objects (`obj/`)

Capability-based object system.

### Files

| File | Purpose |
|------|---------|
| `object.h/object.c` | Base kernel object with reference counting |
| `handle.h/handle.c` | Handle table and capability management |
| `kobject.h` | Kernel object type definitions |
| `rights.h` | Capability rights definitions |
| `namespace.h/namespace.c` | Object namespace (naming service) |

### Kernel Object Types

- **Handle** - Reference to any kernel object with specific rights
- **Process** - Executable context
- **Thread** - Unit of execution
- **VMO** - Virtual Memory Object
- **Channel** - IPC endpoint
- **File** - Filesystem object

### Object Rights

Each handle has rights that control what operations are allowed:
- Duplicate (create new reference)
- Transfer (send to other process)
- Read/Write/Execute (operation-specific)
- Map (memory map capability)

## ELF Loading (`kernel/`)

Binary loading and execution.

### Files

| File | Purpose |
|------|---------|
| `elf64.h/elf64.c` | ELF64 binary format parsing and loading |

### ELF64 Support

- 64-bit executable files (x86-64)
- Program headers for memory layout
- Dynamic linking support
- Entry point and segment loading

## System Calls (`syscall/`)

User-kernel interface.

### Files

| File | Purpose |
|------|---------|
| `syscall.h/syscall.c` | System call dispatcher and handlers |

### Common Syscalls

- Process creation and management
- Memory mapping and VMO operations
- Handle operations (read, write, close, dup)
- Filesystem operations
- Channel creation and messaging
- Scheduling and synchronization

## Boot (`boot/`)

Bootloader interface and initialization data.

### Files

| File | Purpose |
|------|---------|
| `db.h/db.c` | DeltaBoot request handling |
| `request.c` | Boot request processing |

## Utility Libraries (`lib/`)

Kernel utility functions.

### Files

| File | Purpose |
|------|---------|
| `string.h/string.c` | String manipulation |
| `math.h/math.c` | Math functions |
| `io.h/io.c` | I/O operations |
| `path.h/path.c` | Path manipulation |
| `time.h/time.c` | Timing utilities |
| `mem.h/mem.c` | Memory utilities |
| `spinlock.h` | Spinlock synchronization |
| `types.h` | Common type definitions |

## Kernel Entry (`src/`)

Main kernel entry point.

### Files

| File | Purpose |
|------|---------|
| `main.c` | Kernel initialization and entry point |
