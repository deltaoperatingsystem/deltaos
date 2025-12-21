#include "paging.h"
#include <string.h>

//allocate a 4KB-aligned page for page table
static EFI_STATUS alloc_page_table(EFI_BOOT_SERVICES *bs, page_entry_t **table) {
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &addr);
    if (EFI_ERROR(status)) {
        return status;
    }
    *table = (page_entry_t *)addr;
    memset(*table, 0, PAGE_SIZE_4K);
    return EFI_SUCCESS;
}

//get PML4 index (bits 39-47)
static inline uint64_t pml4_index(uint64_t virt) {
    return (virt >> 39) & 0x1FF;
}

//get PDPT index (bits 30-38)
static inline uint64_t pdpt_index(uint64_t virt) {
    return (virt >> 30) & 0x1FF;
}

//get PD index (bits 21-29)
static inline uint64_t pd_index(uint64_t virt) {
    return (virt >> 21) & 0x1FF;
}

//get PT index (bits 12-20)
static inline uint64_t pt_index(uint64_t virt) {
    return (virt >> 12) & 0x1FF;
}

//get or create a table at an entry
static EFI_STATUS get_or_create_table(
    EFI_BOOT_SERVICES *bs,
    page_entry_t *entry,
    page_entry_t **child
) {
    if (*entry & PTE_PRESENT) {
        *child = (page_entry_t *)(*entry & PTE_ADDR_MASK);
        return EFI_SUCCESS;
    }
    
    EFI_STATUS status = alloc_page_table(bs, child);
    if (EFI_ERROR(status)) {
        return status;
    }
    
    *entry = (uint64_t)*child | PTE_PRESENT | PTE_WRITABLE;
    return EFI_SUCCESS;
}

EFI_STATUS paging_init(EFI_BOOT_SERVICES *bs, page_tables_t *pt) {
    EFI_STATUS status;
    
    //allocate PML4
    status = alloc_page_table(bs, &pt->pml4);
    if (EFI_ERROR(status)) {
        return status;
    }
    pt->pml4_phys = (uint64_t)pt->pml4;
    
    return EFI_SUCCESS;
}

EFI_STATUS paging_map_2mb(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t virt,
    uint64_t phys,
    uint64_t flags
) {
    EFI_STATUS status;
    page_entry_t *pdpt, *pd;
    
    //get indices
    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx = pd_index(virt);
    
    //get or create PDPT
    status = get_or_create_table(bs, &pt->pml4[pml4_idx], &pdpt);
    if (EFI_ERROR(status)) return status;
    
    //get or create PD
    status = get_or_create_table(bs, &pdpt[pdpt_idx], &pd);
    if (EFI_ERROR(status)) return status;
    
    //set PD entry as 2MB huge page
    pd[pd_idx] = (phys & ~(PAGE_SIZE_2M - 1)) | flags | PTE_HUGE | PTE_PRESENT;
    
    return EFI_SUCCESS;
}

EFI_STATUS paging_map_4kb(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t virt,
    uint64_t phys,
    uint64_t flags
) {
    EFI_STATUS status;
    page_entry_t *pdpt, *pd, *pt_table;
    
    //get indices
    uint64_t pml4_idx = pml4_index(virt);
    uint64_t pdpt_idx = pdpt_index(virt);
    uint64_t pd_idx = pd_index(virt);
    uint64_t pt_idx = pt_index(virt);
    
    //get or create PDPT
    status = get_or_create_table(bs, &pt->pml4[pml4_idx], &pdpt);
    if (EFI_ERROR(status)) return status;
    
    //get or create PD
    status = get_or_create_table(bs, &pdpt[pdpt_idx], &pd);
    if (EFI_ERROR(status)) return status;

    //get or create PT
    status = get_or_create_table(bs, &pd[pd_idx], &pt_table);
    if (EFI_ERROR(status)) return status;
    
    //set PT entry
    pt_table[pt_idx] = (phys & ~(PAGE_SIZE_4K - 1)) | flags | PTE_PRESENT;
    
    return EFI_SUCCESS;
}

EFI_STATUS paging_identity_map(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    EFI_MEMORY_DESCRIPTOR *mmap,
    UINTN mmap_size,
    UINTN desc_size
) {
    EFI_STATUS status;
    UINTN entry_count = mmap_size / desc_size;
    uint8_t *ptr = (uint8_t *)mmap;
    
    for (UINTN i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)ptr;
        
        //identity map the region
        //uses 2MB pages for the bulk of memory mapping cuz it's efficient
        uint64_t start = desc->PhysicalStart;
        uint64_t end = start + (desc->NumberOfPages * PAGE_SIZE_4K);
        
        //round start down and end up to 2MB boundaries
        uint64_t map_start = start & ~(PAGE_SIZE_2M - 1);
        uint64_t map_end = (end + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);
        
        for (uint64_t addr = map_start; addr < map_end; addr += PAGE_SIZE_2M) {
            status = paging_map_2mb(bs, pt, addr, addr, PTE_WRITABLE);
            if (EFI_ERROR(status)) return status;
        }
        
        ptr += desc_size;
    }
    
    return EFI_SUCCESS;
}

EFI_STATUS paging_map_higher_half(EFI_BOOT_SERVICES *bs, page_tables_t *pt, uint64_t size_gb) {
    EFI_STATUS status;
    uint64_t num_2mb_pages = (size_gb * PAGE_SIZE_1G) / PAGE_SIZE_2M;
    
    for (uint64_t i = 0; i < num_2mb_pages; i++) {
        uint64_t phys = i * PAGE_SIZE_2M;
        uint64_t virt = KERNEL_VMA + phys;
        status = paging_map_2mb(bs, pt, virt, phys, PTE_WRITABLE);
        if (EFI_ERROR(status)) return status;
    }
    
    return EFI_SUCCESS;
}

EFI_STATUS paging_map_kernel(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t virt_base,
    uint64_t phys_base,
    uint64_t size
) {
    EFI_STATUS status;
    
    uint64_t virt_start = virt_base & ~(PAGE_SIZE_4K - 1);
    uint64_t phys_start = phys_base & ~(PAGE_SIZE_4K - 1);
    uint64_t virt_end = (virt_base + size + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    
    uint64_t num_pages = (virt_end - virt_start) / PAGE_SIZE_4K;
    
    //ensure at least some pages
    if (num_pages == 0) num_pages = 16;  //at least 64KB for entry code
    
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t virt = virt_start + i * PAGE_SIZE_4K;
        uint64_t phys = phys_start + i * PAGE_SIZE_4K;
        status = paging_map_4kb(bs, pt, virt, phys, PTE_WRITABLE);
        if (EFI_ERROR(status)) return status;
    }
    
    return EFI_SUCCESS;
}

EFI_STATUS paging_map_framebuffer(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t fb_base,
    uint64_t fb_size
) {
    EFI_STATUS status;
    
    //align to 2MB boundaries for efficiency
    uint64_t start = fb_base & ~(PAGE_SIZE_2M - 1);
    uint64_t end = (fb_base + fb_size + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);
    
    //map each 2MB page with write-combining attributes
    //PTE_NOCACHE + PTE_WRITETHROUGH = write-combining (PAT dependent but safe fallback)
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE_2M) {
        status = paging_map_2mb(bs, pt, addr, addr, 
            PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
        if (EFI_ERROR(status)) return status;
    }
    
    return EFI_SUCCESS;
}

void paging_enable(page_tables_t *pt) {
    //load PML4 into CR3
    __asm__ volatile(
        "mov %0, %%cr3"
        :
        : "r"(pt->pml4_phys)
        : "memory"
    );
}
