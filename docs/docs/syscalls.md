# Syscall Reference

Complete reference for DeltaOS system calls available to user-space programs.

## Overview

System calls are the primary interface between user-space applications and the kernel. All syscalls are accessed through `libc/include/system.h` wrappers.

### Syscall Convention

**x86-64 Calling Convention:**
- Arguments passed in: `rdi, rsi, rdx, rcx, r8, r9`
- Return value in: `rax`
- Syscall invocation: `syscall` instruction
- Syscall number in: `rax`

## Process Control Syscalls

### exit

Terminate current process.

```c
void exit(int code);
```

- **Number:** `SYS_EXIT (0)`
- **Arguments:** `rdi` = exit code
- **Returns:** Never (terminates process)
- **Notes:** Performs cleanup, sets process to DEAD state

### getpid

Get current process ID.

```c
int64 getpid(void);
```

- **Number:** `SYS_GETPID`
- **Arguments:** None
- **Returns:** `rax` = process ID
- **Notes:** Always succeeds

### yield

Yield CPU to scheduler.

```c
void yield(void);
```

- **Number:** `SYS_YIELD`
- **Arguments:** None
- **Returns:** When rescheduled
- **Notes:** Process moves to end of ready queue

### spawn

Create and start child process.

```c
int spawn(char *path, int argc, char **argv);
```

- **Number:** `SYS_SPAWN`
- **Arguments:**
  - `rdi` = path to executable
  - `rsi` = argc
  - `rdx` = argv array
- **Returns:** `rax` = child PID or -1 on error
- **Notes:** Higher-level helper, internally uses process_create/start

## Advanced Process Creation

### process_create

Create suspended process.

```c
int32 process_create(const char *name);
```

- **Number:** `SYS_PROCESS_CREATE`
- **Arguments:** `rdi` = process name pointer
- **Returns:** `rax` = process handle or -1 on error
- **Error Codes:**
  - `-ENOMEM` - No memory for process structure
  - `-EINVAL` - Invalid name
- **Notes:** Process does not start until process_start called

### process_start

Start first thread of suspended process.

```c
int process_start(int32 proc_h, uint64 entry, uint64 stack);
```

- **Number:** `SYS_PROCESS_START`
- **Arguments:**
  - `rdi` = process handle
  - `rsi` = entry point address
  - `rdx` = stack pointer
- **Returns:** `rax` = 0 on success, -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle or process not suspended
  - `-EACCES` - Insufficient rights
- **Notes:** Sets RIP=entry, RSP=stack, adds to ready queue

## Handle & Capability Syscalls

### get_obj

Get handle to object at path.

```c
handle_t get_obj(handle_t parent, const char *path, uint32 rights);
```

- **Number:** `SYS_GET_OBJ (5)`
- **Arguments:**
  - `rdi` = parent handle (INVALID_HANDLE for root)
  - `rsi` = path string pointer
  - `rdx` = requested rights
- **Returns:** `rax` = handle or INVALID_HANDLE on error
- **Requested Rights:**
  - `RIGHT_READ` - Read permission
  - `RIGHT_WRITE` - Write permission
  - `RIGHT_EXECUTE` - Execute permission
  - `RIGHT_MAP` - Memory map permission
  - `RIGHT_DUPLICATE` - Duplicate permission
  - `RIGHT_TRANSFER` - Transfer permission
- **Error Codes:**
  - `-ENOENT` - Path not found
  - `-EACCES` - Access denied
  - `-EINVAL` - Invalid path or parent handle
- **Notes:** Resolves path through mounted filesystems

### handle_grant

Grant capability to process.

```c
int handle_grant(int32 proc_h, int32 local_h, uint32 rights);
```

- **Number:** `SYS_HANDLE_GRANT`
- **Arguments:**
  - `rdi` = target process handle
  - `rsi` = local handle to grant
  - `rdx` = rights to grant (must be subset of local_h rights)
- **Returns:** `rax` = 0 on success, -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle
  - `-EACCES` - Insufficient rights in source handle
  - `-ENOMEM` - Target process handle table full
- **Notes:** Creates new handle in target process

### handle_read

Read from handle.

```c
int handle_read(handle_t h, void *buf, int len);
```

- **Number:** `SYS_HANDLE_READ (6)`
- **Arguments:**
  - `rdi` = handle
  - `rsi` = buffer pointer
  - `rdx` = buffer size
- **Returns:** `rax` = bytes read or -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle
  - `-EACCES` - No READ right
  - `-EIO` - I/O error
  - `-EEOF` - End of file (returns 0)
- **Notes:** Requires RIGHT_READ on handle

### handle_write

Write to handle.

```c
int handle_write(handle_t h, const void *buf, int len);
```

- **Number:** `SYS_HANDLE_WRITE (7)`
- **Arguments:**
  - `rdi` = handle
  - `rsi` = data pointer
  - `rdx` = data size
- **Returns:** `rax` = bytes written or -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle
  - `-EACCES` - No WRITE right
  - `-EIO` - I/O error
  - `-ENOSPC` - No space (storage full)
- **Notes:** Requires RIGHT_WRITE on handle

### handle_close

Close handle.

```c
int handle_close(handle_t h);
```

- **Number:** `SYS_HANDLE_CLOSE (32)`
- **Arguments:** `rdi` = handle
- **Returns:** `rax` = 0 on success, -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle
- **Notes:** Decrements object refcount, may free object if refcount reaches 0

### handle_dup

Duplicate handle with possibly reduced rights.

```c
handle_t handle_dup(handle_t h, uint32 new_rights);
```

- **Number:** `SYS_HANDLE_DUP`
- **Arguments:**
  - `rdi` = handle to duplicate
  - `rsi` = new rights (must be subset of source rights)
- **Returns:** `rax` = new handle or INVALID_HANDLE on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle or rights
  - `-ENOMEM` - Handle table full
- **Notes:** Cannot expand rights, only reduce them

### handle_seek

Seek in handle (for files).

```c
int handle_seek(handle_t h, size offset, int mode);
```

- **Number:** `SYS_HANDLE_SEEK`
- **Arguments:**
  - `rdi` = handle
  - `rsi` = offset
  - `rdx` = seek mode
- **Returns:** `rax` = new position or -1 on error
- **Seek Modes:**
  - `HANDLE_SEEK_SET (0)` - Absolute from start
  - `HANDLE_SEEK_OFF (1)` - Relative to current
  - `HANDLE_SEEK_END (2)` - Relative to end
- **Error Codes:**
  - `-EINVAL` - Invalid handle or mode
  - `-EACCES` - Handle not seekable
- **Notes:** For non-seekable handles (pipes), returns -1

## VMO (Virtual Memory Object) Syscalls

### vmo_create

Create virtual memory object.

```c
handle_t vmo_create(size size, uint32 flags);
```

- **Number:** `SYS_VMO_CREATE (37)`
- **Arguments:**
  - `rdi` = size in bytes
  - `rsi` = flags
- **Returns:** `rax` = VMO handle or INVALID_HANDLE on error
- **Flags:**
  - `VMO_FLAG_NONE (0)` - Fixed size
  - `VMO_FLAG_RESIZABLE (1)` - Can be resized with vmo_resize
- **Error Codes:**
  - `-ENOMEM` - Insufficient memory
  - `-EINVAL` - Invalid size (0 or too large)
- **Notes:** VMO starts zeroed

### vmo_map

Map VMO into address space.

```c
int vmo_map(handle_t h, void *addr, size size, uint32 flags);
```

- **Number:** `SYS_VMO_MAP (40)`
- **Arguments:**
  - `rdi` = VMO handle
  - `rsi` = virtual address to map at
  - `rdx` = size to map
  - `rcx` = mapping flags
- **Returns:** `rax` = 0 on success, -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle or address
  - `-EACCES` - No MAP right
  - `-EEXIST` - Address range already mapped
  - `-ENOMEM` - Insufficient page table memory
- **Notes:** Requires RIGHT_MAP on VMO handle

### vmo_write

Write to VMO.

```c
int vmo_write(handle_t h, size offset, const void *buf, size len);
```

- **Number:** `SYS_VMO_WRITE`
- **Arguments:**
  - `rdi` = VMO handle
  - `rsi` = offset within VMO
  - `rdx` = data pointer
  - `rcx` = data size
- **Returns:** `rax` = bytes written or -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle or offset
  - `-EACCES` - No WRITE right
  - `-ERANGE` - Offset + size exceeds VMO size
- **Notes:** Requires RIGHT_WRITE on VMO handle

### vmo_read

Read from VMO.

```c
int vmo_read(handle_t h, size offset, void *buf, size len);
```

- **Number:** `SYS_VMO_READ`
- **Arguments:**
  - `rdi` = VMO handle
  - `rsi` = offset within VMO
  - `rdx` = buffer pointer
  - `rcx` = buffer size
- **Returns:** `rax` = bytes read or -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle or offset
  - `-EACCES` - No READ right
  - `-ERANGE` - Offset + size exceeds VMO size
- **Notes:** Requires RIGHT_READ on VMO handle

## Channel IPC Syscalls

### channel_create

Create bidirectional channel.

```c
int channel_create(handle_t *ep0, handle_t *ep1);
```

- **Number:** `SYS_CHANNEL_CREATE`
- **Arguments:**
  - `rdi` = pointer to ep0 (out)
  - `rsi` = pointer to ep1 (out)
- **Returns:** `rax` = 0 on success, -1 on error
- **Error Codes:**
  - `-ENOMEM` - Insufficient memory for channel
  - `-EINVAL` - Invalid pointers
- **Notes:** Creates two connected endpoints

### channel_send

Send message on channel.

```c
int channel_send(handle_t h, const void *msg, size len);
```

- **Number:** `SYS_CHANNEL_SEND`
- **Arguments:**
  - `rdi` = channel handle
  - `rsi` = message data pointer
  - `rdx` = message size
- **Returns:** `rax` = 0 on success, -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle
  - `-EACCES` - No WRITE right
  - `-EMSGSIZE` - Message too large (> 65536 bytes)
  - `-EPIPE` - Other end closed
- **Notes:** Wakes waiting receivers

### channel_recv

Receive message from channel.

```c
int channel_recv(handle_t h, void *msg, size len);
```

- **Number:** `SYS_CHANNEL_RECV`
- **Arguments:**
  - `rdi` = channel handle
  - `rsi` = buffer pointer
  - `rdx` = buffer size
- **Returns:** `rax` = bytes received or -1 on error
- **Error Codes:**
  - `-EINVAL` - Invalid handle
  - `-EACCES` - No READ right
  - `-EWOULDBLOCK` - No message available (non-blocking)
  - `-EMSGSIZE` - Message larger than buffer
- **Notes:** Blocks if no message available

## Filesystem Syscalls

### stat

Get file status.

```c
int stat(handle_t h, const char *path, stat_t *st);
```

- **Number:** `SYS_STAT (43)`
- **Arguments:**
  - `rdi` = parent handle (or root if INVALID_HANDLE)
  - `rsi` = path string
  - `rdx` = pointer to stat_t structure
- **Returns:** `rax` = 0 on success, -1 on error
- **Error Codes:**
  - `-ENOENT` - Path not found
  - `-EACCES` - Access denied
  - `-EINVAL` - Invalid path or pointer
- **Notes:** stat_t contains type, size, timestamps

### readdir

Read directory entries.

```c
int readdir(handle_t h, const char *path, dirent_t *entries, 
            uint32 count, uint32 *index);
```

- **Number:** `SYS_READDIR`
- **Arguments:**
  - `rdi` = parent handle
  - `rsi` = directory path
  - `rdx` = entries buffer
  - `rcx` = max entries
  - `r8` = index pointer (in/out)
- **Returns:** `rax` = number of entries read or -1 on error
- **Error Codes:**
  - `-ENOENT` - Directory not found
  - `-ENOTDIR` - Not a directory
  - `-EACCES` - Access denied
- **Notes:** Stateless iteration using index

## Debug Syscalls

### debug_write

Write debug output (kernel console).

```c
int debug_write(const char *buf, int len);
```

- **Number:** `SYS_DEBUG_WRITE (3)`
- **Arguments:**
  - `rdi` = data pointer
  - `rsi` = data size
- **Returns:** `rax` = bytes written
- **Notes:** Always available, useful for debugging

## Error Codes

All syscalls return -errno on failure (where applicable):

| Code | Constant | Meaning |
|------|----------|---------|
| -1 | EPERM | Operation not permitted |
| -2 | ENOENT | No such file or directory |
| -12 | ENOMEM | Out of memory |
| -13 | EACCES | Permission denied |
| -14 | EFAULT | Bad address |
| -16 | EBUSY | Device or resource busy |
| -17 | EEXIST | File exists |
| -22 | EINVAL | Invalid argument |
| -28 | ENOSPC | No space left on device |
| -32 | EPIPE | Broken pipe |
| -35 | EWOULDBLOCK | Resource temporarily unavailable |
| -52 | EMSGSIZE | Message too large |

## Typical Usage Patterns

### Creating a Child Process

```c
// Create process
int32 proc = process_create("app");

// Grant capabilities
handle_t file = get_obj(INVALID_HANDLE, "/data", RIGHT_READ);
handle_grant(proc, file, RIGHT_READ);

// Start it
process_start(proc, entry, stack);

// Wait for it (optional)
wait(proc);
```

### File I/O

```c
// Open
handle_t f = get_obj(INVALID_HANDLE, "/file.txt", RIGHT_READ);

// Read
char buf[256];
int n = handle_read(f, buf, 256);

// Close
handle_close(f);
```

### Memory Sharing

```c
// Create VMO
handle_t vmo = vmo_create(4096, VMO_FLAG_NONE);

// Map it
vmo_map(vmo, (void *)0x1000, 4096, 0);

// Write to it
char data[] = "test";
vmo_write(vmo, 0, data, 4);

// Duplicate for other process
handle_t dup = handle_dup(vmo, RIGHT_READ | RIGHT_MAP);
handle_grant(proc, dup, RIGHT_READ | RIGHT_MAP);
```

### IPC

```c
// Create channel
handle_t ep0, ep1;
channel_create(&ep0, &ep1);

// Transfer to child
handle_grant(child_proc, ep1, RIGHT_READ | RIGHT_WRITE);

// Parent sends
channel_send(ep0, "hello", 5);

// Child receives (in child process)
char buf[256];
int n = channel_recv(ep1, buf, 256);
```
