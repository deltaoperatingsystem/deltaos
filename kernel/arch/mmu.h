#ifndef ARCH_MMU_H
#define ARCH_MMU_H

#include <arch/types.h>

/*
 * architecture-independent mmu interface
 * each architecture provides its implementation in arch/<arch>/mmu.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/mmu.h>
#elif defined(ARCH_X86)
    #error "x86 not implemented"
#elif defined(ARCH_ARM64)
    #error "ARM64 not implemented"
#else
    #error "Unsupported architecture"
#endif

/*
 * required MI functions - each arch must implement:
 *
 * mmu_map_range(map, virt, phys, pages, flags) - map range of pages
 * mmu_unmap_range(map, virt, pages) - unmap range of pages
 * mmu_virt_to_phys(map, virt) - translate virtual address to physical physical
 * mmu_switch(map) - switch to a different address space
 * mmu_get_kernel_pagemap() - get the kernel's initial pagemap
 *
 * required types:
 * pagemap_t - structure representing an address space (page tables)
 *
 * required flags:
 * MMU_FLAG_PRESENT, MMU_FLAG_WRITE, MMU_FLAG_USER, MMU_FLAG_NOCACHE, MMU_FLAG_EXEC
 */

#endif
