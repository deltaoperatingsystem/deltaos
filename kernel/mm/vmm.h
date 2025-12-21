#ifndef MM_VMM_H
#define MM_VMM_H

#include <arch/types.h>
#include <arch/mmu.h>

void vmm_init(void);

//higher-level mapping that handles multiple pages
void vmm_map(pagemap_t *map, uintptr virt, uintptr phys, size pages, uint64 flags);
void vmm_unmap(pagemap_t *map, uintptr virt, size pages);

//kernel-specific mappings
void vmm_kernel_map(uintptr virt, uintptr phys, size pages, uint64 flags);

#endif
