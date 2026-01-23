# Kernel Overview

The DeltaOS kernel is a capability-based microkernel designed for security and modularity. It implements modern microkernel concepts similar to Zircon, with process and memory isolation, capability-based security, and efficient IPC mechanisms.

## Architecture

The kernel is built on **x86-64 (AMD64)** architecture with the following core subsystems:

### Key Components

1. **Architecture Layer** (`arch/`) - CPU, interrupts, and MMU abstractions
2. **Memory Management** (`mm/`) - Physical/virtual memory, heaps, and VMOs
3. **Process Management** (`proc/`) - Processes, threads, and scheduling
4. **Filesystem** (`fs/`) - Virtual filesystem with multiple backends
5. **Device Drivers** (`drivers/`) - Hardware abstraction (keyboard, mouse, graphics, etc.)
6. **IPC** (`ipc/`) - Inter-process communication via channels
7. **Kernel Objects** (`obj/`) - Capability-based object system
8. **System Calls** (`syscall/`) - User-kernel boundary

## Core Concepts

### Capability-Based Security

The kernel uses capability-based security where every access to kernel objects is mediated through handles with specific rights:

- `RIGHT_DUPLICATE` - Duplicate handle
- `RIGHT_TRANSFER` - Transfer handle to another process
- `RIGHT_READ` - Read access
- `RIGHT_WRITE` - Write access
- `RIGHT_EXECUTE` - Execute access
- `RIGHT_MAP` - Memory map access

### Kernel Objects

The kernel manages various object types accessed through capabilities:

- **Processes** - Isolated execution contexts
- **Threads** - Units of execution within a process
- **VMOs** (Virtual Memory Objects) - Memory regions with copy-on-write support
- **Handles** - References to kernel objects with specific rights
- **Channels** - Bidirectional message-passing IPC endpoints

### Virtual Memory

- Supports higher-half kernel mapping (HHDM - Higher Half Direct Map)
- Physical-to-virtual conversion via `P2V()` macro using HHDM offset
- Virtual-to-physical conversion via `V2P()` macro
- Process VMA (Virtual Memory Area) tracking for user space mappings

## Subsystems

### Architecture (`arch/`)

Abstracts CPU-specific functionality for AMD64:

- `cpu.h` - CPU operations and identification
- `context.h/context.c` - Thread context/registers
- `interrupts.h` - Interrupt handling setup
- `io.h` - Port I/O operations
- `mmu.h` - Memory management unit abstractions
- `timer.h` - Timer and tick handling
- `types.h` - Architecture-specific types

### Memory Management (`mm/`)

Manages physical and virtual memory:

- **PMM** (`pmm.h/c`) - Physical Memory Manager - page allocator for physical RAM
- **VMM** (`vmm.h/c`) - Virtual Memory Manager - page table management
- **Kheap** (`kheap.h/c`) - Kernel heap allocator
- **VMO** (`vmo.h/c`) - Virtual Memory Objects - copyable memory regions

### Process Management (`proc/`)

Manages execution:

- **Process** (`process.h/c`) - Process structure with handle tables and VMAs
- **Thread** (`thread.h/c`) - Thread structure and execution contexts
- **Scheduler** (`sched.h/c`) - Process and thread scheduling
- **Wait** (`wait.h/c`) - Process/thread synchronization primitives

Process states: READY, RUNNING, BLOCKED, DEAD, ZOMBIE

### Filesystem (`fs/`)

Provides virtual filesystem abstraction:

- **FS Interface** (`fs.h`) - Filesystem operations (lookup, create, remove, readdir, stat)
- **DA** (`da.h/c`) - DeltaArchive format for read-only archives
- **InitRD** (`initrd.h/c`) - Compressed initial ramdisk filesystem
- **TmpFS** (`tmpfs.h/c`) - In-memory temporary filesystem
- **DIRENT** - Directory entries with names and types
- **STAT** - File status information (type, size, timestamps)

File types: FILE, DIR, SYMLINK, LINK, PIPE, SOCKET, DEVICE

### Drivers (`drivers/`)

Hardware abstraction layer:

- **Console** (`console.h/c`) - Text console output
- **Framebuffer** (`fb.h/c`) - Graphics framebuffer driver
- **Keyboard** (`keyboard.h/c`) - Keyboard input and protocol
- **Mouse** (`mouse.h/c`) - Mouse input and protocol
- **PCI** (`pci.h/c`) - PCI device discovery and management
- **RTC** (`rtc.h/c`) - Real-time clock
- **Serial** (`serial.h/c`) - Serial port I/O
- **VT** - Virtual terminal implementation

### IPC (`ipc/`)

Inter-process communication:

- **Channel** (`channel.h/c`) - Bidirectional message passing endpoints
- **Channel Server** (`channel_server.h`) - Server-side channel management

### Kernel Objects (`obj/`)

Capability-based object system:

- **Object** (`object.h/c`) - Base kernel object structure
- **Handle** (`handle.h/c`) - Handle table management and capabilities
- **KObject** (`kobject.h`) - Kernel object type definitions
- **Rights** (`rights.h`) - Capability rights definitions
- **Namespace** (`namespace.h/c`) - Object namespace management

### ELF Loading (`kernel/`)

- **ELF64** (`elf64.h/c`) - ELF64 binary format parsing and loading

## Syscall Interface

The kernel provides a syscall interface for user-space programs to access kernel functionality. See `syscall/syscall.h` for the complete list of available syscalls.

## Build

The kernel is built with:
```
make -C kernel
```

Produces `kernel/delta.elf` - the kernel executable.
