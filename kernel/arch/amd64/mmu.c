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

static uint64 *get_next_level(uint64 *current_table, uint32 index, bool allocate, bool user);

void mmu_init(void) {
    pagemap_t *map = mmu_get_kernel_pagemap();
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    for (int i = 256; i < 512; i++) {
        get_next_level(pml4, i, true, false);
    }
}

static uint64 *get_next_level(uint64 *current_table, uint32 index, bool allocate, bool user) {
    uint64 entry = current_table[index];
    if (entry & AMD64_PTE_PRESENT) {
        //if we need user access and entry doesn't have it, add it
        if (user && !(entry & AMD64_PTE_USER)) {
            current_table[index] = entry | AMD64_PTE_USER;
        }
        return (uint64 *)P2V(entry & AMD64_PTE_ADDR_MASK);
    }
    
    if (!allocate) return NULL;
    
    //allocate a new page for the next level table
    void *next_table_phys = pmm_alloc(1);
    if (!next_table_phys) return NULL;
    
    uint64 *next_table_virt = (uint64 *)P2V(next_table_phys);
    memset(next_table_virt, 0, PAGE_SIZE);
    
    //set entry in current table to point to new table
    //we set all permissions here as actual permissions are enforced in the leaf PTE
    uint64 flags = AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
    if (user) flags |= AMD64_PTE_USER;
    current_table[index] = (uintptr)next_table_phys | flags;
    
    return next_table_virt;
}

//debug: walk page table for address and print each level
void mmu_debug_walk(pagemap_t *map, uintptr virt, const char *label) {
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    printf("[mmu] walk %s virt=0x%lx PML4=0x%lx\n", label, virt, map->top_level);
    
    uint64 pml4_entry = pml4[PML4_IDX(virt)];
    printf("[mmu]   PML4[%d]=0x%lx\n", PML4_IDX(virt), pml4_entry);
    if (!(pml4_entry & AMD64_PTE_PRESENT)) {
        printf("[mmu]   STOP: PML4 not present\n");
        return;
    }
    
    uint64 *pdp = (uint64 *)P2V(pml4_entry & AMD64_PTE_ADDR_MASK);
    uint64 pdp_entry = pdp[PDP_IDX(virt)];
    printf("[mmu]   PDP[%d]=0x%lx\n", PDP_IDX(virt), pdp_entry);
    if (!(pdp_entry & AMD64_PTE_PRESENT)) {
        printf("[mmu]   STOP: PDP not present\n");
        return;
    }
    
    uint64 *pd = (uint64 *)P2V(pdp_entry & AMD64_PTE_ADDR_MASK);
    uint64 pd_entry = pd[PD_IDX(virt)];
    printf("[mmu]   PD[%d]=0x%lx\n", PD_IDX(virt), pd_entry);
    if (!(pd_entry & AMD64_PTE_PRESENT)) {
        printf("[mmu]   STOP: PD not present\n");
        return;
    }
    if (pd_entry & AMD64_PTE_HUGE) {
        printf("[mmu]   2MB huge page\n");
        return;
    }
    
    uint64 *pt = (uint64 *)P2V(pd_entry & AMD64_PTE_ADDR_MASK);
    uint64 pt_entry = pt[PT_IDX(virt)];
    printf("[mmu]   PT[%d]=0x%lx\n", PT_IDX(virt), pt_entry);
    if (!(pt_entry & AMD64_PTE_PRESENT)) {
        printf("[mmu]   STOP: PT not present\n");
        return;
    }
    printf("[mmu]   mapped to phys=0x%lx\n", pt_entry & AMD64_PTE_ADDR_MASK);
}

void mmu_map_range(pagemap_t *map, uintptr virt, uintptr phys, size pages, uint64 flags) {
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    uint64 pte_flags = AMD64_PTE_PRESENT;
    if (flags & MMU_FLAG_WRITE) pte_flags |= AMD64_PTE_WRITE;
    if (flags & MMU_FLAG_USER)  pte_flags |= AMD64_PTE_USER;
    if (flags & MMU_FLAG_NOCACHE) pte_flags |= (AMD64_PTE_PCD | AMD64_PTE_PWT);
    if (!(flags & MMU_FLAG_EXEC)) pte_flags |= AMD64_PTE_NX;
    
    bool user = (flags & MMU_FLAG_USER) != 0;

    size i = 0;
    while (i < pages) {
        uintptr cur_virt = virt + (i * PAGE_SIZE);
        uintptr cur_phys = phys + (i * PAGE_SIZE);

        uint64 *pdp = get_next_level(pml4, PML4_IDX(cur_virt), true, user);
        if (!pdp) {
            printf("[mmu] ERR: failed to allocate PDP for virt 0x%lx\n", cur_virt);
            return;
        }
        
        uint64 *pd = get_next_level(pdp, PDP_IDX(cur_virt), true, user);
        if (!pd) {
            printf("[mmu] ERR: failed to allocate PD for virt 0x%lx\n", cur_virt);
            return;
        }

        //try to map a 2MB huge page
        if (pages - i >= 512 && (cur_virt % 0x200000 == 0) && (cur_phys % 0x200000 == 0)) {
            pd[PD_IDX(cur_virt)] = (cur_phys & AMD64_PTE_ADDR_MASK) | pte_flags | AMD64_PTE_HUGE;
            i += 512;
        } else {
            uint64 *pt = get_next_level(pd, PD_IDX(cur_virt), true, user);
            if (!pt) {
                printf("[mmu] ERR: failed to allocate PT for virt 0x%lx\n", cur_virt);
                return;
            }
            pt[PT_IDX(cur_virt)] = (cur_phys & AMD64_PTE_ADDR_MASK) | pte_flags;
            i++;
        }
        
        //invalidate TLB for this address
        __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt) : "memory");
    }
}

void mmu_unmap_range(pagemap_t *map, uintptr virt, size pages) {
    /* debug: log unmapping of kernel heap range
    if (virt >= KHEAP_VIRT_START && virt < KHEAP_VIRT_END) {
        // printf("[mmu] unmap virt=0x%lx pages=%zu\n", virt, pages);
    } */
    
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    for (size i = 0; i < pages; ) {
        uintptr cur_virt = virt + (i * PAGE_SIZE);

        uint64 *pdp = get_next_level(pml4, PML4_IDX(cur_virt), false, false);
        if (!pdp) { i++; continue; }
        
        uint64 *pd = get_next_level(pdp, PDP_IDX(cur_virt), false, false);
        if (!pd) { i++; continue; }

        uint64 pd_entry = pd[PD_IDX(cur_virt)];
        if (pd_entry & AMD64_PTE_HUGE) {
            pd[PD_IDX(cur_virt)] = 0;
            i += 512;
        } else {
            uint64 *pt = get_next_level(pd, PD_IDX(cur_virt), false, false);
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
    
    uint64 *pdp = get_next_level(pml4, PML4_IDX(virt), false, false);
    if (!pdp) return 0;
    
    uint64 *pd = get_next_level(pdp, PDP_IDX(virt), false, false);
    if (!pd) return 0;

    uint64 pd_entry = pd[PD_IDX(virt)];
    if (!(pd_entry & AMD64_PTE_PRESENT)) return 0;
    
    if (pd_entry & AMD64_PTE_HUGE) {
        //2MB huge page
        return (pd_entry & AMD64_PTE_ADDR_MASK) + (virt & 0x1FFFFF);
    }

    uint64 *pt = get_next_level(pd, PD_IDX(virt), false, false);
    if (!pt) return 0;

    uint64 pt_entry = pt[PT_IDX(virt)];
    if (!(pt_entry & AMD64_PTE_PRESENT)) return 0;

    return (pt_entry & AMD64_PTE_ADDR_MASK) + (virt & 0xFFF);
}

void mmu_switch(pagemap_t *map) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(map->top_level) : "memory");
}

pagemap_t *mmu_pagemap_create(void) {
    //allocate pagemap structure from kernel heap
    pagemap_t *map = (pagemap_t *)P2V(pmm_alloc(1));
    if (!map) return NULL;
    
    //allocate PML4
    void *pml4_phys = pmm_alloc(1);
    if (!pml4_phys) {
        pmm_free((void *)V2P(map), 1);
        return NULL;
    }
    
    uint64 *pml4 = (uint64 *)P2V(pml4_phys);
    memset(pml4, 0, PAGE_SIZE);
    
    //copy kernel upper-half entries (indices 256-511) from kernel pagemap
    pagemap_t *kernel_map = mmu_get_kernel_pagemap();
    uint64 *kernel_pml4 = (uint64 *)P2V(kernel_map->top_level);
    
    for (int i = 256; i < 512; i++) {
        pml4[i] = kernel_pml4[i];
    }
    
    map->top_level = (uintptr)pml4_phys;
    return map;
}

static void free_page_table_level(uint64 *table, int level) {
    if (level < 1) return;
    
    for (int i = 0; i < 512; i++) {
        uint64 entry = table[i];
        if (!(entry & AMD64_PTE_PRESENT)) continue;
        if (entry & AMD64_PTE_HUGE) continue;
        
        // If we are at level > 1, the entry points to the next page table level
        if (level > 1) {
            uint64 *next = (uint64 *)P2V(entry & AMD64_PTE_ADDR_MASK);
            free_page_table_level(next, level - 1);
            
            // After returning from child, it's safe to free the CHILD table page
            pmm_free((void *)(entry & AMD64_PTE_ADDR_MASK), 1);
        }
        // If level == 1, the entry points to a DATA page.
        // We do NOT free data pages here; that is the responsibility of the VMA system.
    }
}

void mmu_pagemap_destroy(pagemap_t *map) {
    if (!map || !map->top_level) return;
    
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    //only free user-space entries (lower half indices 0-255)
    //don't touch kernel entries (256-511)
    for (int i = 0; i < 256; i++) {
        uint64 entry = pml4[i];
        if (!(entry & AMD64_PTE_PRESENT)) continue;
        
        uint64 *pdp = (uint64 *)P2V(entry & AMD64_PTE_ADDR_MASK);
        free_page_table_level(pdp, 3);  //PDP is level 3
        pmm_free((void *)(entry & AMD64_PTE_ADDR_MASK), 1);
    }
    
    //free PML4 itself
    pmm_free((void *)map->top_level, 1);
    
    //free pagemap structure
    pmm_free((void *)V2P(map), 1);
}
