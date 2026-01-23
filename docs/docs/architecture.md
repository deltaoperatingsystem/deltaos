# System Architecture

DeltaOS is a capability-based microkernel operating system with a clean separation between kernel and user space. This document describes the overall system design and how components interact.

## System Overview

```
┌─────────────────────────────────────────────────────────┐
│                    User Applications                    │
│         (shell, wm, init, coreutils, custom apps)       │
├─────────────────────────────────────────────────────────┤
│                  User Space (libc, ld)                  │
│         (dynamic linker, C library, libkeyboard)        │
├─────────────────────────────────────────────────────────┤
│                   Syscall Interface                     │
├─────────────────────────────────────────────────────────┤
│                   Kernel (delta.elf)                    │
│  ┌─────────────────────────────────────────────────┐   │
│  │  Memory Management  │ Process Management       │   │
│  │  (PMM, VMM, Kheap)  │ (sched, threads, procs)  │   │
│  ├─────────────────────────────────────────────────┤   │
│  │  Filesystem  │ IPC  │ Device Drivers          │   │
│  │  (DA, tmpfs) │(ch)  │ (console, kb, mouse)    │   │
│  ├─────────────────────────────────────────────────┤   │
│  │  Object System  │  Capabilities  │  Boot     │   │
│  │  (kobject)      │  (rights)      │  (db)     │   │
│  └─────────────────────────────────────────────────┘   │
│                 AMD64 Architecture Layer                │
└─────────────────────────────────────────────────────────┘
```

## Key Design Principles

### 1. Capability-Based Security

Every access to kernel objects is mediated through capabilities (handles):

```
Process A                          Kernel                        Process B
┌─────────────┐                ┌──────────────┐            ┌─────────────┐
│  Handle 1   │                │ Kernel Obj1  │            │  Handle 3   │
│  Rights: R  │──────┬─────────│ References   │────────┬───│  Rights: RW │
│  Obj: O1    │      │         │ Count, Type  │        │   │  Obj: O1    │
├─────────────┤      │         └──────────────┘        │   ├─────────────┤
│  Handle 2   │      │              ▲                   │   │  Handle 4   │
│  Rights: RW │      │              │ Refcount          │   │  Rights: R  │
│  Obj: O2    │      └──────────────┼───────────────────┘   │  Obj: O2    │
└─────────────┘                     │                       └─────────────┘
                            (kernel manages refs)
```

- Each handle has specific rights
- Capabilities cannot be forged
- Kernel enforces all access control
- Rights can only be reduced, never expanded

### 2. Microkernel Architecture

Only essential functionality in kernel:

**In Kernel:**
- Process/thread management
- Memory management
- Core filesystem interface
- Hardware abstraction
- IPC channels

**In User Space:**
- System services (init, shell)
- Filesystems (tmpfs)
- Device servers
- Application logic

### 3. Process Isolation

Each process has:
- Isolated virtual address space
- Private handle table
- Separate kernel object references
- Cannot directly access other processes' memory

### 4. Object-Oriented Kernel Design

Uniform object model:

```c
// Base object with reference counting
typedef struct object {
    uint32 type;           // PROCESS, THREAD, VMO, CHANNEL, etc.
    uint32 refcount;       // Reference counting for GC
    void *data;            // Type-specific data
} object_t;
```

Benefits:
- Consistent handle management
- Reference counting prevents use-after-free
- Extensible for new object types

## Component Interaction

### Boot Flow

```
1. UEFI Bootloader (BOOTX64.EFI)
   │
   └─> Loads kernel (delta.elf) into memory
   
2. Kernel Startup
   ├─> Initialize memory (PMM, VMM)
   ├─> Set up CPU (GDT, IDT)
   ├─> Mount filesystem (initrd)
   └─> Create init process
   
3. Init Process
   ├─> Mounts tmpfs for writable storage
   ├─> Starts core services
   └─> Spawns user applications
   
4. User Applications
   ├─> Shell (command interpreter)
   ├─> Window Manager (graphics)
   └─> Other services
```

### Process Creation Flow

```
Parent Process                 Kernel                    Child Process
│                              │                         │
├─> process_create("app") ────>│                         │
│   (returns handle)            │ Create kernel object   │
│                              │ Allocate VAS            │
│<─── proc_handle ─────────────┤ Initialize thread       │
│                              │                         │
├─> handle_grant(proc, fh) ──>│ Inject handle into      │
│   (share capability)          │ child's handle table   │
│                              │                         │
├─> process_start(...) ───────>│ Set thread entry/stack │
│                              │ Add to ready queue      │
│<─── OK ───────────────────────┤                         │
│                              ├─ Scheduler picks up ────>│
│                              │                         ├─> _start
│                              │                         ├─> main()
│<── wait(pid) ─────┐           │                         │
│   (blocks)        │           │ Process exits          │
│                   │           │ Puts in zombie state   │
│                   └───────────│ Wakes waiting parent   │
│                              │                         │
```

### Filesystem Access

```
User Program                    Kernel                    Filesystem
│                              │                         │
├─> get_obj("/etc/config") ──>│ Resolve path via        │
│   (request handle)           │ mounted filesystems    │
│                              │ (initrd, tmpfs, etc)    │
│<─ file_handle ──────────────┤ Return handle           │
│                              │                         │
├─> handle_read(h, buf) ──────>│ Look up object          │
│   (read data)                │ Check rights (READ)     │
│                              │ Call fs->read()        │
│<─ data ───────────────────────│                         │
│                              │ Return data            │
```

### IPC via Channels

```
Process A              Kernel              Process B
│                      │                    │
├─> channel_create() ─>│ Allocate channel   │
│                      │ object with 2 eps  │
│<─ (ep_a, ep_b) ──────┤                    │
│                      │                    │
├─> transfer ep_b ────>│ Move handle to     │
│   to process B        │ process B's table  │
│                      │                    │
│<────────────────────── ep_b ─────────────>│ Process B
│                      │                    │ has ep_b
│                      │                    │
├─> send(msg) ────────>│ Copy message into  │
│                      │ channel buffer     │
│                      │                    │
│                      ├─ Wake waiting ────>├─> recv(msg)
│                      │   readers          │ (was blocked)
│                      │                    │<─ returns msg
│<─ OK ─────────────────┤                    │
```

## Memory Layout

### Kernel Memory Map (64-bit)

```
Virtual Address Space (x86-64)
┌────────────────────────────────────────┐
│                                        │
│  User Space (0x0 - 0x7FFFFFFFFFFFFFFF) │
│  (each process has own VAS)            │
│                                        │
├────────────────────────────────────────┤
│  Kernel High Half                      │
│  (0xFFFF800000000000 - 0xFFFFFFFF...)   │
│  ├─ Kernel code/data                   │
│  ├─ Kernel heap                        │
│  └─ HHDM (physical RAM direct map)     │
│     (phys_addr + HHDM_OFFSET)          │
└────────────────────────────────────────┘
```

### User Process Memory Layout

```
Virtual Address Space (per process)
┌────────────────────────────┐
│  Text (code) - Read/Exec   │
├────────────────────────────┤
│  Initialized Data (rodata) │
├────────────────────────────┤
│  Uninitialized Data (bss)  │
├────────────────────────────┤
│  Heap (malloc/free)        │
│  ↓ grows downward ↓        │
├────────────────────────────┤
│                            │
│  (gap - unmapped)          │
│                            │
├────────────────────────────┤
│  ↑ grows upward ↑          │
│  Stack                     │
├────────────────────────────┤
│  Thread-local storage      │
└────────────────────────────┘
```

## Syscall Interface

The syscall interface is the primary mechanism for user-space to invoke kernel functions.

```c
// syscall.h - defines syscall numbers
#define SYS_EXIT           0
#define SYS_GETPID         1
#define SYS_GET_OBJ        5
#define SYS_HANDLE_READ    6
#define SYS_HANDLE_WRITE   7
#define SYS_HANDLE_CLOSE   32
// ... more syscalls

// User programs call via libc wrappers:
// handle_t get_obj(handle_t parent, const char *path, uint32 rights)
// int handle_read(handle_t h, void *buf, int len)
// ... etc

// Kernel dispatcher processes syscall number and invokes handler
```

## Object Reference Lifecycle

```
Creation
  │
  ├─> object_create()
  │   ├─> allocate object struct
  │   ├─> initialize type-specific data
  │   └─> set refcount = 1
  │
├─> create handle in process
│   └─> increment refcount
│
Usage
  ├─> user program uses handle
  ├─> kernel validates access
  └─> operation performed
│
Handle Transfer
  ├─> send_handle(proc_a, handle, proc_b)
  ├─> create new handle in proc_b
  ├─> increment object refcount
  └─> optionally remove from proc_a
│
Cleanup
  ├─> handle_close(h)
  │   ├─> decrement refcount
  │   ├─> remove from handle table
  │   └─> if refcount == 0, free object
  └─> object_destroy()
      └─> free object memory
```

## Security Model

### Principle of Least Privilege

- Processes start with minimal capabilities
- Parent selectively grants capabilities to children
- Rights cannot be expanded, only reduced
- Each handle has specific rights

### Sandboxing Example

```c
// Parent creates child with limited access
int32 child = process_create("untrusted_app");

// Grant read-only access to /public directory
handle_t pub_dir = get_obj(INVALID_HANDLE, "/public", RIGHT_READ);
handle_grant(child, pub_dir, RIGHT_READ);

// DO NOT grant access to /sensitive or /home

// Start child with only pub_dir capability
process_start(child, entry, stack);

// Child cannot access any files other than via pub_dir
// If child gets compromised, damage is limited
```

## Scheduling

Simple preemptive scheduler:

```
Ready Queue
  ├─ Thread A (priority 50)
  ├─ Thread B (priority 50)  <- running
  └─ Thread C (priority 40)

Timer Interrupt (every 10ms)
  └─> deschedule Thread B
      reschedule Thread A (same priority, round-robin)
      
Thread Yield
  └─> deschedule current
      pick next from ready queue
```

## Build and Deployment

```
Build Artifacts:
├─ kernel/delta.elf      - Kernel executable
├─ user/libc.so          - C runtime library
├─ user/ld.so            - Dynamic linker
├─ user binary executables
│  ├─ /bin/init
│  ├─ /bin/shell
│  ├─ /bin/wm
│  └─ ...
└─ initrd.da             - DeltaArchive with system files

Distribution Image:
└─ os.img
   ├─ BOOTX64.EFI (UEFI bootloader)
   ├─ delta.elf (kernel)
   ├─ initrd.da (system files)
   └─ boot.cfg (boot configuration)
```

## Extension Points

DeltaOS is designed to be extensible:

### Adding New Syscalls

1. Define syscall number in `syscall.h`
2. Implement handler in `syscall.c`
3. Add wrapper function in `libc/include/system.h`

### Adding New Drivers

1. Implement driver interface in `drivers/`
2. Register with kernel at startup
3. Expose via `/dev/drivername` virtual files

### Adding New Filesystems

1. Implement `fs_ops_t` interface in `fs/`
2. Mount at startup or on demand
3. Handle object creation/access

### Adding New Kernel Objects

1. Define object type in `obj/kobject.h`
2. Implement object operations
3. Create syscalls for access
4. Add handle operations
