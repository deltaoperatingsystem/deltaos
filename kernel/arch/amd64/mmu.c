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

static void mmu_write_msr(uint32 msr, uint64 val) {
    uint32 lo = val & 0xFFFFFFFF;
    uint32 hi = val >> 32;
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static uint64 mmu_read_msr(uint32 msr) {
    uint32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64)hi << 32) | lo;
}

static uint64 *get_next_level(uint64 *current_table, uint32 index, bool allocate, bool user);

static void mmu_init_pat(void) {
    //configure PAT (page attribute table)
    //index 0 (PAT=0, PCD=0, PWT=0) -> WB (write back) - default
    //index 1 (PAT=0, PCD=0, PWT=1) -> WT (write through) - default
    //index 2 (PAT=0, PCD=1, PWT=0) -> UC- (uncached minus) - default
    //index 3 (PAT=0, PCD=1, PWT=1) -> UC (uncached) - default
    //index 4 (PAT=1, PCD=0, PWT=0) -> WC (write combining) - custom
    // and so on so for
    
    uint64 pat = mmu_read_msr(MSR_IA32_PAT);
    pat &= ~(0x7ULL << 32); //clear PA4
    pat |= (0x1ULL << 32);  //set PA4 to 0x01 (WC)
    mmu_write_msr(MSR_IA32_PAT, pat);
}

void mmu_init(void) {
    mmu_init_pat();
    
    pagemap_t *map = mmu_get_kernel_pagemap();
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    for (int i = 256; i < 512; i++) {
        get_next_level(pml4, i, true, false);
    }
}

static uint64 *get_next_level(uint64 *current_table, uint32 index, bool allocate, bool user) {
    uint64 entry = current_table[index];
    if (entry & AMD64_PTE_PRESENT) {
        //if this is a huge page, we cannot traverse further
        if (entry & AMD64_PTE_HUGE) return NULL;
        
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
    
    //check for write combining
    bool is_wc = (flags & MMU_FLAG_WC) != 0;
    
    bool user = (flags & MMU_FLAG_USER) != 0;
    //printf("[mmu] map_range virt=0x%lx phys=0x%lx pages=%zu flags=0x%lx\n", virt, phys, pages, flags);

    size i = 0;
    while (i < pages) {
        uintptr cur_virt = virt + (i * PAGE_SIZE);
        uintptr cur_phys = phys + (i * PAGE_SIZE);

        uint64 *pdp = get_next_level(pml4, PML4_IDX(cur_virt), true, user);
        if (!pdp) {
            //if get_next_level failed it might be because the entry is a HUGE page
            //we treat this as a fatal error for now but in kernel space we could overwrite
            printf("[mmu] ERR: failed to traverse PML4 index %d for 0x%lx\n", PML4_IDX(cur_virt), cur_virt);
            return;
        }
        
        uint64 *pd = get_next_level(pdp, PDP_IDX(cur_virt), true, user);
        if (!pd) {
            printf("[mmu] ERR: failed to traverse PDP index %d for 0x%lx\n", PDP_IDX(cur_virt), cur_virt);
            return;
        }

        //try to map a 2MB huge page
        bool mapped_huge = false;
        if (pages - i >= 512 && (cur_virt % 0x200000 == 0) && (cur_phys % 0x200000 == 0)) {
            //overwrite existing entry (whether it was a PT or a huge page)
            uint64 huge_flags = pte_flags | AMD64_PTE_HUGE;
            if (is_wc) {
                //for huge pages (2MB), PAT bit is bit 12
                huge_flags |= (1ULL << 12);
                //make sure PCD/PWT are clear
                huge_flags &= ~(AMD64_PTE_PCD | AMD64_PTE_PWT);
            }
            
            pd[PD_IDX(cur_virt)] = (cur_phys & AMD64_PTE_ADDR_MASK) | huge_flags;
            
            //thoroughly invalidate the entire 2MB range in TLB
            for (int j = 0; j < 512; j++) {
                __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt + j * 4096) : "memory");
            }
            
            i += 512;
            mapped_huge = true;
        }
        
        if (!mapped_huge) {
            //if the PD entry is already a HUGE page we must split/overwrite it to map a 4KB page
            if (pd[PD_IDX(cur_virt)] & AMD64_PTE_HUGE) {
                 // ... splitting logic same as before ...       
                uint64 old_entry = pd[PD_IDX(cur_virt)];
                uintptr base_phys = old_entry & AMD64_PTE_ADDR_MASK;
                uint64 flags = old_entry & ~AMD64_PTE_ADDR_MASK & ~AMD64_PTE_HUGE;

                //allocate a new PT
                void *pt_phys = pmm_alloc(1);
                if (!pt_phys) {
                    printf("[mmu] ERR: failed to allocate PT for split\n");
                    return;
                }
                uint64 *pt_virt = (uint64 *)P2V(pt_phys);
                
                //populate the new PT with 4KB entries
                for (int j = 0; j < 512; j++) {
                    pt_virt[j] = (base_phys + (j * 4096)) | flags;
                }

                //update the PD entry to point to the new PT
                pd[PD_IDX(cur_virt)] = (uintptr)pt_phys | AMD64_PTE_PRESENT | AMD64_PTE_WRITE | (user ? AMD64_PTE_USER : 0);
                
                //invalidate the original huge page
                __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt & ~0x1FFFFFULL) : "memory");
            }

            uint64 *pt = get_next_level(pd, PD_IDX(cur_virt), true, user);
            if (!pt) {
                printf("[mmu] ERR: failed to allocate PT for virt 0x%lx\n", cur_virt);
                return;
            }
            
            uint64 leaf_flags = pte_flags;
            if (is_wc) {
                //for 4KB pages, PAT bit is bit 7 (AMD64_PTE_PAT)
                leaf_flags |= AMD64_PTE_PAT;
                //make sure PCD/PWT are clear
                leaf_flags &= ~(AMD64_PTE_PCD | AMD64_PTE_PWT);
            }
            
            pt[PT_IDX(cur_virt)] = (cur_phys & AMD64_PTE_ADDR_MASK) | leaf_flags;
            __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt) : "memory");
            i++;
        }
    }
    
    
    //check that all pages are actually mapped
    for (size v = 0; v < pages; v++) {
        uintptr check_virt = virt + (v * PAGE_SIZE);
        uintptr resolved = mmu_virt_to_phys(map, check_virt);
        if (resolved == 0) {
            printf("[mmu] VERIFY FAIL: page %zu at 0x%lx not mapped!\\n", v, check_virt);
        }
    }
}

void mmu_unmap_range(pagemap_t *map, uintptr virt, size pages) {
    /*debug: log unmapping of kernel heap range
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
            //invalidate all 512 pages in the huge block
            for (int j = 0; j < 512; j++) {
                __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt + j * 4096) : "memory");
            }
            i += 512;
        } else {
            uint64 *pt = get_next_level(pd, PD_IDX(cur_virt), false, false);
            if (pt) {
                pt[PT_IDX(cur_virt)] = 0;
            }
            __asm__ volatile ("invlpg (%0)" :: "r"(cur_virt) : "memory");
            i++;
        }
    }
}

uintptr mmu_virt_to_phys(pagemap_t *map, uintptr virt) {
    uint64 *pml4 = (uint64 *)P2V(map->top_level);
    
    uint64 *pdp = get_next_level(pml4, PML4_IDX(virt), false, false);
    if (!pdp) return 0;
    
    uint64 *pd = get_next_level(pdp, PDP_IDX(virt), false, false);
    if (!pd) {
        //check if the PD itself has a huge page entry (get_next_level returns NULL for huge)
        uint64 entry = pdp[PDP_IDX(virt)];
        if ((entry & AMD64_PTE_PRESENT) && (entry & AMD64_PTE_HUGE)) {
            return (entry & AMD64_PTE_ADDR_MASK) + (virt & 0x1FFFFF);
        }
        return 0;
    }

    uint64 pd_entry = pd[PD_IDX(virt)];
    if ((pd_entry & AMD64_PTE_PRESENT) && (pd_entry & AMD64_PTE_HUGE)) {
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
