#include <arch/amd64/mmu.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <lib/string.h>
#include <lib/io.h>

static pagemap_t kernel_pagemap;

pagemap_t *mmu_get_kernel_pagemap(void) {
    if (kernel_pagemap.top_level == 0) {
        //retrieve current PML4 from CR3 on first call
        uintptr cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        kernel_pagemap.top_level = cr3;
    }
    return &kernel_pagemap;
}

static uint64 *get_next_level(uint64 *current_table, uint32 index, bool allocate) {
    uint64 entry = current_table[index];
    if (entry & AMD64_PTE_PRESENT) {
        return (uint64 *)P2V(entry & AMD64_PTE_ADDR_MASK);
    }
    
    if (!allocate) return NULL;
    
    //allocate a new page for the next level table
    void *next_table_phys = pmm_alloc(1);
    if (!next_table_phys) return NULL;
    
    uint64 *next_table_virt = (uint64 *)P2V(next_table_phys);
    memset(next_table_virt, 0, PAGE_SIZE);
    
    //set entry in current table to point to new table
    //we allow all permissions here asactual permissions are enforced in the leaf PTE
    current_table[index] = (uintptr)next_table_phys | AMD64_PTE_PRESENT | AMD64_PTE_WRITE | AMD64_PTE_USER;
    
    return next_table_virt;
}

void mmu_map_range(pagemap_t *map, uintptr virt, uintptr phys, size pages, uint64 flags) {
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    uint64 pte_flags = AMD64_PTE_PRESENT;
    if (flags & MMU_FLAG_WRITE) pte_flags |= AMD64_PTE_WRITE;
    if (flags & MMU_FLAG_USER)  pte_flags |= AMD64_PTE_USER;
    if (flags & MMU_FLAG_NOCACHE) pte_flags |= (AMD64_PTE_PCD | AMD64_PTE_PWT);
    if (!(flags & MMU_FLAG_EXEC)) pte_flags |= AMD64_PTE_NX; 

    size i = 0;
    while (i < pages) {
        uintptr cur_virt = virt + (i * PAGE_SIZE);
        uintptr cur_phys = phys + (i * PAGE_SIZE);

        uint64 *pdp = get_next_level(pml4, PML4_IDX(cur_virt), true);
        if (!pdp) return;
        
        uint64 *pd = get_next_level(pdp, PDP_IDX(cur_virt), true);
        if (!pd) return;

        //try to map a 2MB huge page
        if (pages - i >= 512 && (cur_virt % 0x200000 == 0) && (cur_phys % 0x200000 == 0)) {
            pd[PD_IDX(cur_virt)] = (cur_phys & AMD64_PTE_ADDR_MASK) | pte_flags | AMD64_PTE_HUGE;
            i += 512;
        } else {
            uint64 *pt = get_next_level(pd, PD_IDX(cur_virt), true);
            if (!pt) return;
            pt[PT_IDX(cur_virt)] = (cur_phys & AMD64_PTE_ADDR_MASK) | pte_flags;
            i++;
        }
        
        //invalidate TLB for this address
        __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt) : "memory");
    }
}

void mmu_unmap_range(pagemap_t *map, uintptr virt, size pages) {
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    for (size i = 0; i < pages; ) {
        uintptr cur_virt = virt + (i * PAGE_SIZE);

        uint64 *pdp = get_next_level(pml4, PML4_IDX(cur_virt), false);
        if (!pdp) { i++; continue; }
        
        uint64 *pd = get_next_level(pdp, PDP_IDX(cur_virt), false);
        if (!pd) { i++; continue; }

        uint64 pd_entry = pd[PD_IDX(cur_virt)];
        if (pd_entry & AMD64_PTE_HUGE) {
            pd[PD_IDX(cur_virt)] = 0;
            i += 512;
        } else {
            uint64 *pt = get_next_level(pd, PD_IDX(cur_virt), false);
            if (pt) {
                pt[PT_IDX(cur_virt)] = 0;
            }
            i++;
        }
        
        __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt) : "memory");
    }
}

uintptr mmu_virt_to_phys(pagemap_t *map, uintptr virt) {
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    uint64 *pdp = get_next_level(pml4, PML4_IDX(virt), false);
    if (!pdp) return 0;
    
    uint64 *pd = get_next_level(pdp, PDP_IDX(virt), false);
    if (!pd) return 0;

    uint64 pd_entry = pd[PD_IDX(virt)];
    if (!(pd_entry & AMD64_PTE_PRESENT)) return 0;
    
    if (pd_entry & AMD64_PTE_HUGE) {
        //2MB huge page
        return (pd_entry & AMD64_PTE_ADDR_MASK) + (virt & 0x1FFFFF);
    }

    uint64 *pt = get_next_level(pd, PD_IDX(virt), false);
    if (!pt) return 0;

    uint64 pt_entry = pt[PT_IDX(virt)];
    if (!(pt_entry & AMD64_PTE_PRESENT)) return 0;

    return (pt_entry & AMD64_PTE_ADDR_MASK) + (virt & 0xFFF);
}

void mmu_switch(pagemap_t *map) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(map->top_level) : "memory");
}
