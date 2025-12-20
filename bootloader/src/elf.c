#include "elf.h"
#include <string.h>

int elf_validate(const void *data, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) {
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
    if (ehdr->e_phoff + (ehdr->e_phnum * ehdr->e_phentsize) > size) {
        return 0;
    }
    
    return 1;
}

EFI_STATUS elf_load(
    EFI_BOOT_SERVICES *bs,
    const void *elf_data,
    uint64_t elf_size,
    uint64_t *entry_point,
    elf_load_info_t *load_info
) {
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    const uint8_t *base = (const uint8_t *)elf_data;
    
    *entry_point = ehdr->e_entry;
    
    uint64_t min_vaddr = ~0ULL;
    uint64_t max_vaddr = 0;
    
    //calculate total size
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        
        if (phdr->p_vaddr < min_vaddr) min_vaddr = phdr->p_vaddr;
        if (phdr->p_vaddr + phdr->p_memsz > max_vaddr) max_vaddr = phdr->p_vaddr + phdr->p_memsz;
    }
    
    if (min_vaddr == ~0ULL) return EFI_LOAD_ERROR;
    
    //align to page boundaries
    uint64_t align_mask = 0xFFF;
    uint64_t load_size = (max_vaddr - min_vaddr + align_mask) & ~align_mask;
    uint64_t aligned_min_vaddr = min_vaddr & ~align_mask;
    uint64_t offset_adjustment = min_vaddr - aligned_min_vaddr;
    load_size += offset_adjustment; //add padding for alignment
    
    //ensure load_size is page aligned
    load_size = (load_size + align_mask) & ~align_mask;
    
    uint64_t num_pages = load_size / 4096;
    
    //allocate contiguous pages
    EFI_PHYSICAL_ADDRESS alloc_addr;
    EFI_STATUS status = bs->AllocatePages(
        AllocateAnyPages,
        EfiLoaderData,
        num_pages,
        &alloc_addr
    );
    
    if (EFI_ERROR(status)) return status;
    
    //zero the allocated memory
    memset((void*)alloc_addr, 0, load_size);
    
    //update load_info
    if (load_info) {
        load_info->phys_base = alloc_addr;
        load_info->phys_end = alloc_addr + load_size;
        load_info->virt_base = aligned_min_vaddr;
        load_info->virt_entry = ehdr->e_entry;
    }
    
    //load segments
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        
        //calculate storage location
        //phys_addr = alloc_addr + (virt_addr - aligned_min_vaddr)
        uint64_t offset = phdr->p_vaddr - aligned_min_vaddr;
        uint8_t *dest = (uint8_t *)(alloc_addr + offset);
        
        //copy file data
        if (phdr->p_filesz > 0) {
            memcpy(dest, base + phdr->p_offset, phdr->p_filesz);
        }
        
        //zero BSS (remaining memsz)
        if (phdr->p_memsz > phdr->p_filesz) {
            memset(dest + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
        }
    }
    
    //update entry point if not higher half
    //if virt base is high we assume Higher Half and don't adjust entry point (it remains virtual)
    if (aligned_min_vaddr < 0xFFFFFFFF80000000ULL) {
       *entry_point = alloc_addr + (ehdr->e_entry - aligned_min_vaddr);
    }
    
    return EFI_SUCCESS;
}

