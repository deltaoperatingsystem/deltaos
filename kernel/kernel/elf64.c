#include "elf64.h"
#include <mm/pmm.h>
#include <string.h>

int elf_validate(const void *data, size len) {
    if (len < sizeof(Elf64_Ehdr)) {
        return 0;
    }
    
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    
    //check magic
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return 0;
    }
    
    //must be 64-bit
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return 0;
    }
    
    //must be little-endian
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return 0;
    }
    
    //must be executable or shared object (PIE)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return 0;
    }
    
    //must be amd64
    if (ehdr->e_machine != EM_X86_64) {
        return 0;
    }
    
    //check program header table bounds
    if (ehdr->e_phoff + (ehdr->e_phnum * ehdr->e_phentsize) > len) {
        return 0;
    }
    
    return 1;
}

int elf_load(const void *data, size len, elf_load_info_t *info) {
    if (!elf_validate(data, len)) {
        return ELF_ERR_INVALID;
    }
    
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    const uint8 *base = (const uint8 *)data;
    
    uint64 min_vaddr = ~0ULL;
    uint64 max_vaddr = 0;
    
    //first pass: calculate total size needed
    for (uint16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        
        if (phdr->p_vaddr < min_vaddr) min_vaddr = phdr->p_vaddr;
        if (phdr->p_vaddr + phdr->p_memsz > max_vaddr) max_vaddr = phdr->p_vaddr + phdr->p_memsz;
    }
    
    if (min_vaddr == ~0ULL) {
        return ELF_ERR_NO_SEGMENTS;
    }
    
    //align to page boundaries
    uint64 align_mask = PAGE_SIZE - 1;
    uint64 aligned_min_vaddr = min_vaddr & ~align_mask;
    uint64 offset_adjustment = min_vaddr - aligned_min_vaddr;
    uint64 load_size = (max_vaddr - min_vaddr + offset_adjustment + align_mask) & ~align_mask;
    
    size pages = load_size / PAGE_SIZE;
    
    //allocate physical pages
    void *alloc = pmm_alloc(pages);
    if (!alloc) {
        return ELF_ERR_NO_MEMORY;
    }
    
    uint64 alloc_addr = (uint64)alloc;
    
    //zero the memory
    memset(alloc, 0, load_size);
    
    //second pass: load segments
    for (uint16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        
        //calculate destination: alloc_addr + (virt_addr - aligned_min_vaddr)
        uint64 offset = phdr->p_vaddr - aligned_min_vaddr;
        uint8 *dest = (uint8 *)(alloc_addr + offset);
        
        //copy file data
        if (phdr->p_filesz > 0) {
            memcpy(dest, base + phdr->p_offset, phdr->p_filesz);
        }
    }
    
    //fill in load info
    if (info) {
        info->phys_base = alloc_addr;
        info->phys_size = load_size;
        info->virt_base = aligned_min_vaddr;
        info->pages = pages;
        
        //determine entry point
        //if higher-half kernel, keep virtual entry
        //otherwise, convert to physical
        if (aligned_min_vaddr >= 0xFFFFFFFF80000000ULL) {
            info->entry = ehdr->e_entry;  //virtual
        } else {
            info->entry = alloc_addr + (ehdr->e_entry - aligned_min_vaddr);  //physical
        }
    }
    
    return ELF_OK;
}

void elf_unload(elf_load_info_t *info) {
    if (info && info->phys_base && info->pages > 0) {
        pmm_free((void *)info->phys_base, info->pages);
        info->phys_base = 0;
        info->pages = 0;
    }
}
