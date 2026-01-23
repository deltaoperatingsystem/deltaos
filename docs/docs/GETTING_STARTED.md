# Getting Started with DeltaOS Documentation

Welcome to the DeltaOS documentation! This guide will help you navigate and get the most out of these resources.

## What is DeltaOS?

DeltaOS is a capability-based microkernel operating system featuring:

- **Microkernel Architecture** - Minimal kernel, most services in user space
- **Capability-Based Security** - Fine-grained access control via unforgeable references
- **Process Isolation** - Each process has isolated memory and capabilities
- **IPC-First Design** - Message-passing as primary inter-process communication
- **x86-64 Support** - Designed for 64-bit AMD64 processors
- **Modern Features** - Virtual memory objects, dynamic linking, filesystem abstraction

## Documentation Organization

The documentation is organized by role and component:

```
docs/docs/
├── INDEX.md                 ← Start here (master index)
├── README.md                ← This file (getting started)
├── architecture.md          ← System-wide design
├── syscalls.md              ← System call reference
├── kernel/                  ← Kernel documentation
│   ├── overview.md          ← What is the kernel?
│   └── subsystems.md        ← Detailed reference
├── user/                    ← User space documentation
│   ├── overview.md          ← What is user space?
│   └── libraries.md         ← API reference
└── tools/                   ← Development tools
    └── overview.md          ← Tool documentation
```

## Choose Your Path

### Path 1: I'm New to DeltaOS

1. Read [INDEX.md](INDEX.md) - Get the big picture
2. Read [architecture.md](architecture.md) - Understand the overall design
3. Pick your specific role below

### Path 2: I'm a Kernel Developer

**Learn the kernel:**
1. [kernel/overview.md](kernel/overview.md) - Architecture and concepts
2. [kernel/subsystems.md](kernel/subsystems.md) - Detailed subsystems
3. [syscalls.md](syscalls.md) - System call interface
4. [architecture.md](architecture.md) - How it all fits together

**Start developing:**
- Modify kernel code in `kernel/` directory
- Refer to subsystems documentation for specific areas
- Use syscall reference to understand kernel-user interface

### Path 3: I'm a User Space Developer

**Learn user space:**
1. [user/overview.md](user/overview.md) - Build system and components
2. [user/libraries.md](user/libraries.md) - API reference
3. [syscalls.md](syscalls.md) - Low-level interface
4. [architecture.md](architecture.md) - System interaction

**Start developing:**
- Write applications in `user/src/` directory
- Use libc headers in `user/libc/include/`
- Refer to examples in libraries.md
- Check architecture.md for patterns

### Path 4: I'm a System Designer

**Understand the system:**
1. [architecture.md](architecture.md) - Overall design
2. [kernel/overview.md](kernel/overview.md) - Kernel design
3. [user/overview.md](user/overview.md) - User space design
4. [INDEX.md](INDEX.md) - Extension points

**Design extensions:**
- Refer to extension points in architecture.md
- Check subsystems documentation
- Review syscall reference

### Path 5: I'm Using Development Tools

**Learn the tools:**
1. [tools/overview.md](tools/overview.md) - Tool documentation
2. [user/overview.md](user/overview.md#build-system) - Build system
3. [INDEX.md](INDEX.md) - For general build info

**Use the tools:**
- `db_patch.py` for boot database patching
- `darc` for archive creation
- See tools/overview.md for detailed usage

## Key Concepts Explained

### Capabilities

A **capability** is an unforgeable reference to a kernel object with specific rights.

```c
// Request capability for reading a file
handle_t file = get_obj(INVALID_HANDLE, "/data.txt", RIGHT_READ);

// Capabilities can be passed to child processes
handle_grant(child_proc, file, RIGHT_READ);

// Or duplicated with reduced rights
handle_t readonly = handle_dup(file, RIGHT_READ);
```

**Why?** Prevents unauthorized access - the kernel enforces rights.

### Process Isolation

Each process is isolated in its own virtual address space and cannot directly access other processes' memory.

```c
// Parent creates child with limited access
int32 child = process_create("app");
handle_t dir = get_obj(INVALID_HANDLE, "/app/data", RIGHT_READ | RIGHT_WRITE);
handle_grant(child, dir, RIGHT_READ | RIGHT_WRITE);
process_start(child, entry, stack);

// Child can ONLY access /app/data and kernel-provided capabilities
// Cannot escape sandbox
```

### IPC via Channels

Processes communicate using message-passing channels.

```c
// Create bidirectional channel
handle_t ep0, ep1;
channel_create(&ep0, &ep1);

// Transfer ep1 to child process
handle_grant(child, ep1, RIGHT_READ | RIGHT_WRITE);

// Parent sends message
channel_send(ep0, "hello", 5);

// Child receives (in child process)
char buf[256];
int n = channel_recv(ep1, buf, 256);
```

### Virtual Address Space

Each process has its own virtual address space (VAS) that maps to physical memory through page tables.

```c
// Create and map virtual memory object
handle_t vmo = vmo_create(4096, 0);
vmo_map(vmo, (void *)0x1000, 4096, 0);

// Now 0x1000-0x2000 in this process refers to the VMO
// Other processes see different memory at same addresses
```

## Common Tasks

### Writing a Simple Program

```c
#include <system.h>
#include <io.h>

int main(int argc, char *argv[]) {
    puts("Hello, DeltaOS!\n");
    
    // Use capabilities
    handle_t file = get_obj(INVALID_HANDLE, "/etc/motd", RIGHT_READ);
    if (file != INVALID_HANDLE) {
        char buf[256];
        int n = handle_read(file, buf, sizeof(buf));
        printf("Read %d bytes\n", n);
        handle_close(file);
    }
    
    return 0;
}
```

**See:** [user/libraries.md](user/libraries.md) for available functions

### Creating a Child Process

```c
// Create suspended process
int32 proc = process_create("child");

// Grant capabilities (read-only home directory)
handle_t home = get_obj(INVALID_HANDLE, "/home", RIGHT_READ | RIGHT_MAP);
handle_grant(proc, home, RIGHT_READ | RIGHT_MAP);

// Start the process
process_start(proc, entry_point, stack_pointer);

// Wait for completion
wait(proc);
```

**See:** [user/libraries.md - Process Control](user/libraries.md#process-control)

### Communicating Between Processes

```c
// Create channel
handle_t ep0, ep1;
channel_create(&ep0, &ep1);

// Transfer ep1 to child process (via process creation)
int32 child = process_create("server");
handle_grant(child, ep1, RIGHT_READ | RIGHT_WRITE);
process_start(child, entry, stack);

// In parent: send request
char request[] = "get_status";
channel_send(ep0, request, strlen(request));

// In parent: receive response
char response[256];
int n = channel_recv(ep0, response, sizeof(response));

// In child (server): receive request
char buf[256];
int n = channel_recv(ep1, buf, sizeof(buf));

// In child: send response
char reply[] = "status_ok";
channel_send(ep1, reply, strlen(reply));
```

**See:** [user/libraries.md - Channel IPC](user/libraries.md#channel-ipc)

### Accessing Files

```c
#include <system.h>
#include <io.h>

// Open file for reading
handle_t file = get_obj(INVALID_HANDLE, "/path/to/file", RIGHT_READ);
if (file == INVALID_HANDLE) {
    printf("Error: cannot open file\n");
    return -1;
}

// Read data
char buf[512];
while (1) {
    int n = handle_read(file, buf, sizeof(buf));
    if (n <= 0) break;  // EOF or error
    
    // Process n bytes from buf
    for (int i = 0; i < n; i++) {
        putc(buf[i]);
    }
}

// Close when done
handle_close(file);
```

**See:** [user/libraries.md - Handle Operations](user/libraries.md#handle-operations)

## Finding Information

### By Component

| Component | Start Here |
|-----------|-----------|
| Memory Management | [kernel/subsystems.md#memory-management](kernel/subsystems.md#memory-management) |
| Processes/Threads | [kernel/subsystems.md#process-management](kernel/subsystems.md#process-management) |
| Filesystem | [kernel/subsystems.md#filesystem](kernel/subsystems.md#filesystem) |
| Drivers | [kernel/subsystems.md#drivers](kernel/subsystems.md#drivers) |
| C Library | [user/libraries.md](user/libraries.md) |
| Linker | [user/libraries.md#dynamic-linker-ldso](user/libraries.md#dynamic-linker-ldso) |
| System Calls | [syscalls.md](syscalls.md) |

### By Topic

| Topic | Start Here |
|-------|-----------|
| Security | [architecture.md#security-model](architecture.md#security-model) |
| Capabilities | [architecture.md#1-capability-based-security](architecture.md#1-capability-based-security) |
| Memory Layout | [architecture.md#memory-layout](architecture.md#memory-layout) |
| Boot Process | [architecture.md#boot-flow](architecture.md#boot-flow) |
| Process Creation | [architecture.md#process-creation-flow](architecture.md#process-creation-flow) |
| IPC | [architecture.md#ipc-via-channels](architecture.md#ipc-via-channels) |
| Building | [user/overview.md#build-system](user/overview.md#build-system) |

### By File/Code

| Looking For | Start Here |
|-------------|-----------|
| `kernel/mm/` | [kernel/subsystems.md#memory-management](kernel/subsystems.md#memory-management) |
| `kernel/proc/` | [kernel/subsystems.md#process-management](kernel/subsystems.md#process-management) |
| `kernel/fs/` | [kernel/subsystems.md#filesystem](kernel/subsystems.md#filesystem) |
| `kernel/obj/` | [kernel/subsystems.md#kernel-objects](kernel/subsystems.md#kernel-objects) |
| `user/libc/` | [user/libraries.md](user/libraries.md) |
| `user/ld/` | [user/libraries.md#dynamic-linker-ldso](user/libraries.md#dynamic-linker-ldso) |
| `tools/` | [tools/overview.md](tools/overview.md) |

## Tips for Reading

1. **Use Ctrl+F** to search within documents
2. **Follow Links** - Documents cross-reference related content
3. **Check Examples** - Most sections include practical code samples
4. **Read Progressively** - Start with overview, dig into details as needed
5. **Keep Architecture Handy** - [architecture.md](architecture.md) ties everything together

## Common Questions

**Q: How do I build DeltaOS?**
A: See [user/overview.md#build-system](user/overview.md#build-system) for build instructions.

**Q: How do I write a kernel driver?**
A: See [kernel/subsystems.md#drivers](kernel/subsystems.md#drivers) for driver interface.

**Q: How do I create a child process?**
A: See [user/libraries.md#process-control](user/libraries.md#process-control) and [architecture.md#process-creation-flow](architecture.md#process-creation-flow).

**Q: How do I use capabilities securely?**
A: See [architecture.md#security-model](architecture.md#security-model) and [architecture.md#sandboxing-example](architecture.md#sandboxing-example).

**Q: What system calls are available?**
A: See [syscalls.md](syscalls.md) for complete reference.

**Q: How does IPC work?**
A: See [architecture.md#ipc-via-channels](architecture.md#ipc-via-channels) and [user/libraries.md#channel-ipc](user/libraries.md#channel-ipc).

## Next Steps

1. **Explore your role's documentation** based on your path above
2. **Try examples** from the documentation
3. **Examine actual code** in the repository
4. **Cross-reference** between documents to understand interactions
5. **Build and test** locally before committing changes

## Need More Help?

- **Code Examples:** See each section's "Typical Usage" or "Usage Example"
- **API Details:** Check [syscalls.md](syscalls.md) and [user/libraries.md](user/libraries.md)
- **Architecture:** Review [architecture.md](architecture.md) for system-wide patterns
- **Build Issues:** See [user/overview.md#build-system](user/overview.md#build-system)
- **Specific Component:** Search [INDEX.md](INDEX.md) for component-specific docs

---

Happy exploring! Start with [INDEX.md](INDEX.md) or jump to your role's documentation above.
