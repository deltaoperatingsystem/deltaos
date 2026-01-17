#include <sys/syscall.h>
#include <types.h>

//create a virtual memory object
//size: size in bytes
//flags: VMO flags (0 for none)
//rights: capability rights
int32 vmo_create(uint64 size, uint32 flags, uint32 rights) {
    return __syscall3(SYS_VMO_CREATE, (long)size, (long)flags, (long)rights);
}

//read from a VMO
int vmo_read(int32 h, void *buf, uint64 len, uint64 offset) {
    return __syscall4(SYS_VMO_READ, (long)h, (long)buf, (long)len, (long)offset);
}

//write to a VMO
int vmo_write(int32 h, const void *buf, uint64 len, uint64 offset) {
    return __syscall4(SYS_VMO_WRITE, (long)h, (long)buf, (long)len, (long)offset);
}

//map a VMO into the process address space
//flags: 1=read, 2=write, 3=read+write
//returns mapped address or 0 on failure
void *vmo_map(int32 h, void *vaddr_hint, uint64 offset, uint64 len, uint32 flags) {
    return (void *)__syscall5(SYS_VMO_MAP, (long)h, (long)vaddr_hint, (long)offset, (long)len, (long)flags);
}

//unmap memory from address space
int vmo_unmap(void *vaddr, uint64 len) {
    return __syscall2(SYS_VMO_UNMAP, (long)vaddr, (long)len);
}

//resize a VMO
int vmo_resize(int32 h, uint64 new_size) {
    return __syscall2(SYS_VMO_RESIZE, (long)h, (long)new_size);
}
