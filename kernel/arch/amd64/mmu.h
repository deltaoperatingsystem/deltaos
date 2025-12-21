#ifndef ARCH_AMD64_MMU_H
#define ARCH_AMD64_MMU_H

#include <arch/amd64/types.h>

//MMU flags
#define MMU_FLAG_PRESENT    (1ULL << 0)
#define MMU_FLAG_WRITE      (1ULL << 1)
#define MMU_FLAG_USER       (1ULL << 2)
#define MMU_FLAG_NOCACHE    (1ULL << 3)
#define MMU_FLAG_EXEC       (1ULL << 4)

//amd64 page table entry bits
#define AMD64_PTE_PRESENT   (1ULL << 0)
#define AMD64_PTE_WRITE     (1ULL << 1)
#define AMD64_PTE_USER      (1ULL << 2)
#define AMD64_PTE_PWT       (1ULL << 3)
#define AMD64_PTE_PCD       (1ULL << 4)
#define AMD64_PTE_ACCESSED  (1ULL << 5)
#define AMD64_PTE_DIRTY     (1ULL << 6)
#define AMD64_PTE_HUGE      (1ULL << 7)
#define AMD64_PTE_GLOBAL    (1ULL << 8)
#define AMD64_PTE_NX        (1ULL << 63)

#define AMD64_PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

//amd64 virtual address space layout
#define HHDM_OFFSET      0xFFFF800000000000ULL
#define KHEAP_VIRT_START 0xFFFF900000000000ULL
#define KHEAP_VIRT_END   0xFFFFA00000000000ULL

typedef struct pagemap {
    uintptr top_level; //physical address of PML4
} pagemap_t;

//helpers to get indices
#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDP_IDX(v)  (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

//MI mmu interface
void mmu_map_range(pagemap_t *map, uintptr virt, uintptr phys, size pages, uint64 flags);
void mmu_unmap_range(pagemap_t *map, uintptr virt, size pages);
uintptr mmu_virt_to_phys(pagemap_t *map, uintptr virt);
void mmu_switch(pagemap_t *map);
pagemap_t *mmu_get_kernel_pagemap(void);

#endif
