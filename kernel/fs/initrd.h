#ifndef FS_INITRD_H
#define FS_INITRD_H

//initialize initrd: parse DA archive from boot info and populate tmpfs
//files are mounted under /initrd/
void initrd_init(void);

//get initrd base address (for reclamation later)
void *initrd_get_base(void);

//get initrd size in bytes
unsigned long long initrd_get_size(void);

//free initrd memory back to PMM (call after data is copied to tmpfs)
void initrd_reclaim(void);

#endif
