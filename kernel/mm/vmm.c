#include <mm/vmm.h>
#include <mm/pmm.h>
#include <arch/mmu.h>
#include <lib/io.h>

void vmm_map(pagemap_t *map, uintptr virt, uintptr phys, size pages, uint64 flags) {
    mmu_map_range(map, virt, phys, pages, flags);
}

void vmm_unmap(pagemap_t *map, uintptr virt, size pages) {
    mmu_unmap_range(map, virt, pages);
}

void vmm_kernel_map(uintptr virt, uintptr phys, size pages, uint64 flags) {
    vmm_map(mmu_get_kernel_pagemap(), virt, phys, pages, flags);
}

void vmm_init(void) {
    pagemap_t *kernel_map = mmu_get_kernel_pagemap();
    printf("[vmm] initializing kernel address space (PML4: 0x%x)\n", kernel_map->top_level);
    
    //the kernel is already mapped by the bootloader (HHDM + Kernel ELF)
    //vmm_init can eventually set up heap guards whatever
}
