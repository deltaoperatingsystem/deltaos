#ifndef _PAGING_H
#define _PAGING_H

#include <stdint.h>
#include "efi.h"

//page table entry flags
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_WRITETHROUGH (1ULL << 3)
#define PTE_NOCACHE     (1ULL << 4)
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_HUGE        (1ULL << 7)  //2MB page in PD, 1GB page in PDPT
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)

//address masks
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define PAGE_SIZE_4K    0x1000
#define PAGE_SIZE_2M    0x200000
#define PAGE_SIZE_1G    0x40000000

//higher-half virtual base (canonical -2GB)
#define KERNEL_VMA      0xFFFFFFFF80000000ULL

//page table structure (512 entries, 4KB each level)
typedef uint64_t page_entry_t;

typedef struct {
    page_entry_t *pml4;         //PML4 table (root)
    uint64_t pml4_phys;         //physical address of PML4
} page_tables_t;

//initialize page tables
//allocates all required structures and sets up
//identity mapping for 0-4GB and higher-half mapping based on kernel load info
EFI_STATUS paging_init(EFI_BOOT_SERVICES *bs, page_tables_t *pt);

//map a 2MB region: virt -> phys
EFI_STATUS paging_map_2mb(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t virt,
    uint64_t phys,
    uint64_t flags
);

//map a 4KB region: virt -> phys
EFI_STATUS paging_map_4kb(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t virt,
    uint64_t phys,
    uint64_t flags
);

//set up identity mapping from UEFI memory map
EFI_STATUS paging_identity_map(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    EFI_MEMORY_DESCRIPTOR *mmap,
    UINTN mmap_size,
    UINTN desc_size
);

//set up higher-half mapping: KERNEL_VMA -> 0 physical
EFI_STATUS paging_map_higher_half(EFI_BOOT_SERVICES *bs, page_tables_t *pt, uint64_t size_gb);

//map kernel: virt_base -> phys_base for size bytes (2MB aligned)
EFI_STATUS paging_map_kernel(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t virt_base,
    uint64_t phys_base,
    uint64_t size
);

//map framebuffer region (identity mapping for MMIO)
EFI_STATUS paging_map_framebuffer(
    EFI_BOOT_SERVICES *bs,
    page_tables_t *pt,
    uint64_t fb_base,
    uint64_t fb_size
);

//load page tables into CR3 (call after ExitBootServices)
void paging_enable(page_tables_t *pt);

#endif
