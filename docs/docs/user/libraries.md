# User Space Libraries Reference

## Standard C Library (libc)

The libc provides standard C functionality for user programs running on DeltaOS.

### Building libc

```bash
make -C user libc.so
```

Produces `user/libc.so` - shared library for runtime linking.

### CRT0 (C Runtime Startup)

Location: `libc/src/crt0.S`

The C runtime startup code:
- Initializes registers and stack
- Processes kernel aux vector
- Sets up argc/argv
- Calls main() function
- Calls exit() with return value

### Type Definitions (`libc/include/types.h`)

Basic types for all programs:

- `int8_t, int16_t, int32_t, int64_t` - Signed integers
- `uint8_t, uint16_t, uint32_t, uint64_t` - Unsigned integers
- `size` / `size_t` - Size type
- `intptr`, `uintptr` - Pointer-sized integers
- `handle_t` - Handle type (`int32`)

### System Calls (`libc/include/sys/syscall.h`)

Raw syscall interface with syscall numbers:

| Syscall | Number | Purpose |
|---------|--------|---------|
| `SYS_EXIT` | 0 | Exit process |
| `SYS_DEBUG_WRITE` | 3 | Write debug output |
| `SYS_GET_OBJ` | 5 | Get handle to object |
| `SYS_HANDLE_READ` | 6 | Read from handle |
| ... | ... | (see full syscall.h) |

### I/O Library (`libc/include/io.h`)

Formatted I/O functions:

- `putc(c)` - Write single character
- `puts(s)` - Write string with newline
- `printf(fmt, ...)` - Formatted output
- `sprintf(buf, fmt, ...)` - Formatted to buffer
- `gets(buf)` - Read line from stdin
- `getc()` - Read character

### String Library (`libc/include/string.h`)

String manipulation:

- `strlen(s)` - String length
- `strcpy(dst, src)` - Copy string
- `strncpy(dst, src, n)` - Copy n bytes
- `strcat(dst, src)` - Concatenate strings
- `strcmp(s1, s2)` - Compare strings
- `strchr(s, c)` - Find character
- `strstr(s, sub)` - Find substring
- `memcpy(dst, src, n)` - Copy memory
- `memset(ptr, val, n)` - Fill memory
- `memcmp(a, b, n)` - Compare memory

### Memory Library (`libc/include/mem.h`)

Dynamic memory allocation:

- `malloc(size)` - Allocate memory
- `free(ptr)` - Free allocated memory
- `realloc(ptr, size)` - Resize allocation
- `calloc(count, size)` - Allocate zeroed memory

### Math Library (`libc/include/math.h`)

Basic math functions:

- `abs(x)` - Absolute value
- `min(a, b)` - Minimum
- `max(a, b)` - Maximum
- `pow(base, exp)` - Power
- `sqrt(x)` - Square root
- `sin(x), cos(x), tan(x)` - Trigonometric

### Argument Parsing (`libc/include/args.h`)

Command-line argument handling:

- `argc` - Argument count
- `argv` - Argument array
- `parse_args(argc, argv)` - Parse flags and options

### Process Control (`libc/include/system.h`)

Process and thread management:

**Process Lifecycle:**
- `exit(code)` - Terminate with exit code
- `getpid()` - Get current process ID
- `yield()` - Yield CPU to scheduler
- `spawn(path, argc, argv)` - Create child process

**Advanced Process Creation:**
- `process_create(name)` - Create suspended process, returns handle
- `handle_grant(proc_h, local_h, rights)` - Grant capability to child process
- `process_start(proc_h, entry, stack)` - Start first thread of suspended process

**Synchronization:**
- `wait(pid)` - Block until process terminates

### Handle Operations

**Basic Operations:**
- `handle_read(h, buf, len)` - Read data from handle
- `handle_write(h, buf, len)` - Write data to handle
- `handle_close(h)` - Close and release handle
- `handle_dup(h, new_rights)` - Duplicate handle with possibly reduced rights

**Seeking:**
- `handle_seek(h, offset, mode)` - Seek in file
  - `HANDLE_SEEK_SET` - Absolute position
  - `HANDLE_SEEK_OFF` - Current position offset
  - `HANDLE_SEEK_END` - Relative to end

**Object Access:**
- `get_obj(parent, path, rights)` - Resolve path and get handle with specified rights

### VMO (Virtual Memory Objects)

Memory operations:

- `vmo_create(size, flags)` - Create VMO
  - `VMO_FLAG_NONE` - Standard VMO
  - `VMO_FLAG_RESIZABLE` - Can be resized
- `vmo_map(h, addr, size, flags)` - Map VMO at virtual address
- `vmo_write(h, offset, buf, len)` - Write to VMO at offset
- `vmo_read(h, offset, buf, len)` - Read from VMO at offset

### Channel IPC

**Channel Creation:**
- `channel_create(ep0, ep1)` - Create bidirectional channel

**Message Passing:**
- `channel_send(h, msg, len)` - Send message on channel
- `channel_recv(h, msg, len)` - Receive message from channel

## Keyboard Library (libkeyboard)

Optional library for applications needing keyboard input.

### Building libkeyboard

```bash
make -C user/libkeyboard
```

### Features

- Keyboard event reading
- Protocol handling
- Key code parsing
- Special key support (Shift, Ctrl, Alt)

### Usage

Applications like shell and window manager link with libkeyboard:

```c
#include <keyboard.h>

int main() {
    keyboard_event_t event;
    while (read_keyboard_event(&event)) {
        // Handle keyboard event
    }
}
```

## Dynamic Linker (ld.so)

The dynamic linker resolves symbols at runtime for shared libraries.

### Building

```bash
make -C user/ld
```

Produces `user/ld.so` - the dynamic linker executable.

### ELF Support

Handles ELF64 binaries with:
- Program headers (PT_LOAD, PT_DYNAMIC, etc.)
- Dynamic linking table (DT_* entries)
- Symbol resolution via SYMTAB
- Relocation processing

### Aux Vector

Kernel passes information to linker via aux vector:

| Tag | Purpose |
|-----|---------|
| `AT_PHDR` | Program header address |
| `AT_PHENT` | Program header entry size |
| `AT_PHNUM` | Program header count |
| `AT_ENTRY` | Entry point address |

### Relocation Types

- `R_X86_64_64` - Absolute 64-bit address
- `R_X86_64_GLOB_DAT` - Global symbol reference
- `R_X86_64_JUMP_SLOT` - PLT relocation
- `R_X86_64_RELATIVE` - PC-relative relocation

## Core Utilities (coreutils)

Command-line tools for file and stream operations.

### cat

Concatenate and display file contents.

```c
// libc/include/system.h usage
handle_t file = get_obj(INVALID_HANDLE, argv[1], RIGHT_READ);
char buf[512];
int len;
while ((len = handle_read(file, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < len; i++) putc(buf[i]);
}
handle_close(file);
```

### echo

Print text to output.

```c
printf("Usage: echo [text]\n");
```

### ls

List directory contents.

```c
// Reads directory entries via handle_read
// Displays file names and types
```

## Compilation Process

### Typical Program Build

1. **Compile source:**
   ```bash
   gcc -ffreestanding -fno-builtin -m64 -c -O2 -std=c11 -fPIC \
       -Ilibc/include program.c -o program.o
   ```

2. **Link with libc:**
   ```bash
   ld -nostdlib -T link.ld program.o libc.a -o program
   ```

3. **Optional: Link with dynamic linker:**
   ```bash
   ld -nostdlib -T link.ld -dynamic-linker ld.so \
       program.o libc.so ld.so -o program
   ```

### Include Paths

- `libc/include` - Standard headers
- `libkeyboard/include` - Keyboard-specific headers (for apps needing it)

### Linker Scripts

- `link.ld` - Standard user space linking
- `libc/src/crt0.S` - Runtime startup
- `ld/entry.S` - Dynamic linker entry

## Security Model

### Capabilities

Programs request capabilities (rights) when accessing objects:

```c
// Read-only access
handle_t ro = get_obj(parent, path, RIGHT_READ);

// Read-write access
handle_t rw = get_obj(parent, path, RIGHT_READ | RIGHT_WRITE);

// Executable access
handle_t exec = get_obj(parent, path, RIGHT_EXECUTE);
```

### Handle Isolation

- Each process has isolated handle table
- Capabilities cannot be forged
- Rights can only be reduced, not expanded
- Kernel mediates all access

### Process Isolation

- Separate virtual address spaces
- Cannot access other processes' memory directly
- IPC via channels only
