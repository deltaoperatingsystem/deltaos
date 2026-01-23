# Documentation Index

Complete documentation for DeltaOS kernel, user space, and development tools.

## System Overview

- [System Architecture](architecture.md) - Overall design, component interaction, security model

## Kernel Documentation

### Overview & Reference
- [Kernel Overview](kernel/overview.md) - High-level kernel architecture and concepts
- [Kernel Subsystems Reference](kernel/subsystems.md) - Detailed reference of all kernel modules

### Key Components

**Memory Management**
- Physical Memory Manager (PMM) - Page allocation and tracking
- Virtual Memory Manager (VMM) - Page table management
- Kernel Heap (Kheap) - Dynamic kernel memory
- Virtual Memory Objects (VMO) - Shareable memory regions

**Process & Thread Management**
- Process structure and lifecycle
- Thread scheduling and execution
- Synchronization primitives
- Process states and transitions

**Filesystem**
- Virtual filesystem interface
- DeltaArchive format (read-only)
- Initial ramdisk (initrd)
- Temporary filesystem (tmpfs)

**Device Drivers**
- Console and framebuffer
- Keyboard and mouse
- PCI bus management
- Real-time clock and serial

**IPC & Objects**
- Channel-based message passing
- Capability-based security
- Kernel object system
- Handle management

## User Space Documentation

### Overview & Libraries
- [User Space Overview](user/overview.md) - Applications, build system, core components
- [User Space Libraries Reference](user/libraries.md) - Detailed API documentation

### C Library (libc)
- Standard type definitions
- System call interface
- I/O operations
- String and memory manipulation
- Process control
- Handle operations
- VMO and channel APIs

### Dynamic Linker (ld)
- ELF64 binary loading
- Dynamic symbol resolution
- Relocation support
- Aux vector processing

### Core Applications

**Utilities (coreutils)**
- `cat` - Display file contents
- `echo` - Print text
- `ls` - List directories

**System Services**
- `init` - System initialization
- `shell` - Command interpreter
- `wm` - Window manager

### Input Libraries
- Keyboard library - Keyboard input handling

## Development Tools

### Tools Documentation
- [Tools Overview](tools/overview.md) - Build and utility tools

### Specific Tools

**db_patch.py**
- DeltaBoot database checksum patching
- Header validation and repair

**darc (DeltaArchive)**
- Archive creation and manipulation
- Format specification
- File packing and extraction

## Quick References

### By Task

#### Building
- See [Kernel Overview - Build](kernel/overview.md#build)
- See [User Space Overview - Build System](user/overview.md#build-system)

#### Understanding Process Creation
- See [System Architecture - Process Creation Flow](architecture.md#process-creation-flow)
- See [User Space Libraries - Process Control](user/libraries.md#process-control)

#### Using Capabilities
- See [System Architecture - Capability-Based Security](architecture.md#1-capability-based-security)
- See [User Space Libraries - Capability-Based Security](user/libraries.md#capability-based-security)

#### IPC & Channels
- See [System Architecture - IPC via Channels](architecture.md#ipc-via-channels)
- See [User Space Libraries - Channel IPC](user/libraries.md#channel-ipc)

#### Memory Operations
- See [Kernel Subsystems - Memory Management](kernel/subsystems.md#memory-management)
- See [User Space Libraries - VMO](user/libraries.md#vmo-virtual-memory-objects)

#### File Access
- See [System Architecture - Filesystem Access](architecture.md#filesystem-access)
- See [User Space Libraries - Handle Operations](user/libraries.md#handle-operations)

### By Component

#### Architecture Layer
- CPU abstractions, interrupts, memory management units
- See [Kernel Subsystems - Architecture](kernel/subsystems.md#architecture)

#### Memory Subsystem
- PMM, VMM, Kheap, VMO
- See [Kernel Subsystems - Memory Management](kernel/subsystems.md#memory-management)
- See [System Architecture - Memory Layout](architecture.md#memory-layout)

#### Filesystem
- Virtual filesystem interface, multiple backends
- See [Kernel Subsystems - Filesystem](kernel/subsystems.md#filesystem)
- See [Tools - DeltaArchive](tools/overview.md#deltaarchive-tool-darc)

#### Drivers
- Console, graphics, input devices, hardware
- See [Kernel Subsystems - Drivers](kernel/subsystems.md#drivers)

#### Object System
- Capability-based handles, kernel objects
- See [Kernel Subsystems - Kernel Objects](kernel/subsystems.md#kernel-objects)
- See [System Architecture - Object Reference Lifecycle](architecture.md#object-reference-lifecycle)

#### IPC
- Channel-based message passing
- See [Kernel Subsystems - IPC](kernel/subsystems.md#ipc)

#### User Libraries
- C library, dynamic linker, utilities
- See [User Space Libraries](user/libraries.md)

## API Quick Start

### Process Creation (Capability-Based)

```c
#include <system.h>

// Create suspended process
int32 proc = process_create("myapp");

// Grant capabilities (read-only file access)
handle_t file = get_obj(INVALID_HANDLE, "/data.txt", RIGHT_READ);
handle_grant(proc, file, RIGHT_READ);

// Start process
process_start(proc, entry_point, stack);

// Wait for completion
int status = wait(getpid());
```

### File Access

```c
#include <system.h>
#include <io.h>

// Open file for reading
handle_t file = get_obj(INVALID_HANDLE, "/etc/config", RIGHT_READ);
if (file == INVALID_HANDLE) {
    printf("Cannot open file\n");
    return;
}

// Read data
char buf[256];
int len = handle_read(file, buf, sizeof(buf));
if (len > 0) {
    printf("Read %d bytes\n", len);
}

// Close when done
handle_close(file);
```

### IPC via Channels

```c
#include <system.h>

// Create channel pair
handle_t ep0, ep1;
channel_create(&ep0, &ep1);

// Transfer ep1 to child process
// ... (via handle_grant or process creation)

// Send message on ep0
channel_send(ep0, "hello", 5);

// Receive on ep1 (in child)
char buf[256];
int len = channel_recv(ep1, buf, sizeof(buf));
```

### Memory Operations

```c
#include <system.h>

// Create virtual memory object
handle_t vmo = vmo_create(4096, VMO_FLAG_NONE);

// Map into address space
vmo_map(vmo, (void *)0x1000, 4096, 0);

// Write data to VMO
char data[] = "test data";
vmo_write(vmo, 0, data, sizeof(data));

// Can share with other processes
handle_t dup = handle_dup(vmo, RIGHT_READ | RIGHT_MAP);
```

## File Organization

```
docs/docs/
├── architecture.md         ← System-wide design
├── kernel/
│   ├── overview.md        ← Kernel intro
│   └── subsystems.md      ← Detailed reference
├── user/
│   ├── overview.md        ← User space intro
│   └── libraries.md       ← API reference
└── tools/
    └── overview.md        ← Tools documentation
```

## Where to Start

1. **New to DeltaOS?** → [System Architecture](architecture.md)
2. **Developing kernel code?** → [Kernel Overview](kernel/overview.md) → [Kernel Subsystems](kernel/subsystems.md)
3. **Writing user programs?** → [User Space Overview](user/overview.md) → [User Libraries](user/libraries.md)
4. **Using build tools?** → [Tools Overview](tools/overview.md)
5. **Understanding security?** → [System Architecture - Security Model](architecture.md#security-model)
6. **Learning IPC?** → [System Architecture - IPC](architecture.md#ipc-via-channels) and [User Libraries - Channels](user/libraries.md#channel-ipc)

## Key Design Concepts

- **Capability-Based Security** - Access control via unforgeable references with specific rights
- **Microkernel** - Minimal kernel, most services in user space
- **Process Isolation** - Each process has isolated virtual address space and handle table
- **Object-Oriented Kernel** - Uniform object model with reference counting
- **IPC-First** - Message-passing as primary inter-process communication
- **Layered Architecture** - Architecture abstraction enables porting to different CPUs

See [System Architecture](architecture.md) for detailed explanation of each.
