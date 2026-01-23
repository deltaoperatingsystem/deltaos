# User Space Overview

User space contains all user-level applications and libraries for DeltaOS. This includes the C library (libc), dynamic linker, core utilities, and system applications.

## Directory Structure

```
user/
├── libc/               # Standard C library
│   ├── include/        # Public headers
│   └── src/            # Implementation
├── libkeyboard/        # Keyboard library (optional for apps)
├── ld/                 # Dynamic linker
├── coreutils/          # Core utilities (cat, echo, ls)
├── src/                # User applications
│   ├── app/            # Application framework
│   ├── init/           # System initialization process
│   ├── shell/          # Shell interpreter
│   └── wm/             # Window manager
└── link.ld             # User space linker script
```

## Build System

The user space uses a hierarchical make system:

- Top-level `Makefile` orchestrates all builds
- Each application has its own directory under `src/`
- Applications can include optional `.config` files for custom flags
- Output binaries placed in `../initrd/system/binaries/`

### Build Targets

| Target | Purpose |
|--------|---------|
| `all` | Build all user binaries and utilities |
| `ld` | Build the dynamic linker |
| `libkeyboard` | Build keyboard input library |
| `libc.so` | Build C library |
| `coreutils` | Build core utilities (cat, echo, ls) |

### Compilation Flags

```
CFLAGS = -ffreestanding -fno-builtin -fno-stack-protector -m64 -O2 -std=c11 -fPIC
```

- Freestanding environment (no OS dependencies)
- No built-in functions (custom implementations)
- Stack protection disabled (not needed in microkernel)
- 64-bit compilation
- Position-independent code (for dynamic linking)

## Core Components

### C Library (libc)

The standard C library provides essential functions for user applications.

#### Include Files (`libc/include/`)

| Header | Purpose |
|--------|---------|
| `types.h` | Basic type definitions |
| `system.h` | System calls and process control |
| `io.h` | I/O operations (read, write, printf) |
| `string.h` | String manipulation |
| `mem.h` | Memory management |
| `math.h` | Math functions |
| `args.h` | Argument parsing |
| `sys/syscall.h` | Raw syscall numbers |

#### System Calls (`libc/include/system.h`)

Process control:
- `exit(code)` - Terminate process
- `getpid()` - Get process ID
- `yield()` - Yield to scheduler
- `spawn(path, argc, argv)` - Create process
- `wait(pid)` - Wait for process termination

Capability-based process creation:
- `process_create(name)` - Create suspended process
- `handle_grant(proc_h, local_h, rights)` - Grant capability to child
- `process_start(proc_h, entry, stack)` - Start child process

Object access:
- `get_obj(parent, path, rights)` - Get handle to object
- `handle_read(h, buf, len)` - Read from handle
- `handle_write(h, buf, len)` - Write to handle
- `handle_close(h)` - Close handle
- `handle_dup(h, new_rights)` - Duplicate with new rights
- `handle_seek(h, offset, mode)` - Seek in handle

Channel IPC:
- `channel_create(ep0, ep1)` - Create channel endpoints
- `channel_send(h, msg, len)` - Send message
- `channel_recv(h, msg, len)` - Receive message

Memory operations:
- `vmo_create(size, flags)` - Create virtual memory object
- `vmo_map(h, addr, size, flags)` - Map VMO into address space
- `vmo_write(h, offset, buf, len)` - Write to VMO

Handle Rights:
- `RIGHT_NONE` - No permissions
- `RIGHT_DUPLICATE` - Duplicate handle
- `RIGHT_TRANSFER` - Transfer to other process
- `RIGHT_READ` - Read access
- `RIGHT_WRITE` - Write access
- `RIGHT_EXECUTE` - Execute access
- `RIGHT_MAP` - Memory map access

### Dynamic Linker (ld/)

The dynamic linker handles runtime relocation and symbol resolution for shared objects.

#### Files

| File | Purpose |
|------|---------|
| `ld.h` | Linker structures and syscall definitions |
| `ld.c` | Main linker implementation |
| `entry.S` | Assembly entry point |
| `link.ld` | Linker script for ld.so |
| `ld.so` | Built dynamic linker executable |

#### Features

- ELF64 binary loading with relocation
- Dynamic symbol resolution
- Support for DT_RELA relocations
- PLT (Procedure Linkage Table) support
- INIT/FINI array execution
- Aux vector parsing for executable information

#### Relocation Types Supported

- `R_X86_64_64` - 64-bit absolute relocation
- `R_X86_64_GLOB_DAT` - Global data reference
- `R_X86_64_JUMP_SLOT` - PLT entry relocation
- `R_X86_64_RELATIVE` - Relative relocation

### Keyboard Library (libkeyboard/)

Provides keyboard input handling for interactive applications.

#### Features

- Keyboard event reading
- Protocol handling for input
- Integration with keyboard driver

### Core Utilities (coreutils/)

Basic command-line utilities.

#### Utilities

| Utility | Purpose |
|---------|---------|
| `cat` | Concatenate and display files |
| `echo` | Print text |
| `ls` | List directory contents |

### System Applications (src/)

#### App Framework (`src/app/`)

Common infrastructure for applications:
- Entry point and initialization
- Argument parsing
- Standard I/O setup

#### Init Process (`src/init/`)

System initialization:
- Started by bootloader
- Initializes core services
- Spawns other system processes (shell, window manager)
- Process supervision

#### Shell (`src/shell/`)

Command interpreter:
- Command parsing and execution
- Pipe support
- Built-in commands
- Environment variables
- Keyboard input (requires libkeyboard)

#### Window Manager (`src/wm/`)

Graphical window manager:
- Window creation and management
- Event handling
- Graphics rendering
- Keyboard and mouse input
- Requires libkeyboard for input handling

## Linking

User space programs are linked using the standard GNU linker script (`link.ld`):

### Typical Build Process

1. Compile C source files to object files with `-fPIC`
2. Link with `ld -nostdlib` to avoid C library dependency
3. Use `libc.so` for runtime support
4. Link with dynamic linker (`ld.so`) for symbol resolution

### Entry Point

User programs start at `_start` which:
1. Processes aux vector from kernel
2. Calls dynamic linker if needed
3. Initializes C runtime (crt0)
4. Calls main()
5. Exits with return code

## Running User Programs

Programs are launched via:

```c
handle_t proc = process_create("myapp");
handle_grant(proc, file_handle, RIGHT_READ | RIGHT_WRITE);
process_start(proc, entry_point, stack_pointer);
```

Or via higher-level spawn():

```c
spawn("/bin/myapp", argc, argv);
```

## Special Features

### Capability-Based Security

User processes request access through capabilities:

```c
handle_t file = get_obj(INVALID_HANDLE, "/path/to/file", RIGHT_READ);
if (file != INVALID_HANDLE) {
    char buf[512];
    handle_read(file, buf, sizeof(buf));
    handle_close(file);
}
```

### Inter-Process Communication

Processes communicate via channels:

```c
handle_t ep0, ep1;
channel_create(&ep0, &ep1);
// ep0 owned by this process, ep1 transferred to another
```

### Memory Operations

VMO-based memory sharing:

```c
handle_t vmo = vmo_create(4096, VMO_FLAG_RESIZABLE);
vmo_map(vmo, (void *)0x1000, 4096, 0);
vmo_write(vmo, 0, data, sizeof(data));
```
