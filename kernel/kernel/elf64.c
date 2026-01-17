#include "elf64.h"
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <string.h>
#include <drivers/serial.h>
#include <proc/process.h>

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
    
    //zero the memory (via HHDM for physical access)
    memset(P2V(alloc), 0, load_size);
    
    //second pass: load segments
    for (uint16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        
        //calculate destination: alloc_addr + (virt_addr - aligned_min_vaddr)
        uint64 offset = phdr->p_vaddr - aligned_min_vaddr;
        uint8 *dest = (uint8 *)P2V(alloc_addr + offset);
        
        //copy file data
        if (phdr->p_filesz > 0) {
            memcpy(dest, base + phdr->p_offset, phdr->p_filesz);
        }
    }
    
    //fill in load info
    if (info) {
        info->phys_base = alloc_addr;
        info->pages = pages;
        info->virt_base = aligned_min_vaddr;
        info->virt_end = max_vaddr;
        info->segment_count = 0;  //kernel load doesn't track segments
        
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

int elf_load_user(const void *data, size len, process_t *proc, elf_load_info_t *info) {
    if (!elf_validate(data, len)) {
        return ELF_ERR_INVALID;
    }
    
    if (!proc || !proc->pagemap || !info) {
        return ELF_ERR_INVALID;
    }
    
    pagemap_t *pagemap = proc->pagemap;
    
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    const uint8 *base = (const uint8 *)data;
    
    //count loadable segments first
    uint32 load_count = 0;
    uint64 min_vaddr = ~0ULL;
    uint64 max_vaddr = 0;
    
    for (uint16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        
        load_count++;
        if (phdr->p_vaddr < min_vaddr) min_vaddr = phdr->p_vaddr;
        if (phdr->p_vaddr + phdr->p_memsz > max_vaddr) max_vaddr = phdr->p_vaddr + phdr->p_memsz;
    }
    
    if (load_count == 0) {
        return ELF_ERR_NO_SEGMENTS;
    }
    
    if (load_count > ELF_MAX_SEGMENTS) {
        return ELF_ERR_TOO_MANY;
    }
    
    //clear info
    memset(info, 0, sizeof(elf_load_info_t));
    info->virt_base = min_vaddr;
    info->virt_end = max_vaddr;
    info->entry = ehdr->e_entry;
    
    //save program header info for aux vector
    info->phdr_count = ehdr->e_phnum;
    info->phdr_size = ehdr->e_phentsize;
    
    //look for PT_INTERP and PT_PHDR
    for (uint16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type == PT_INTERP) {
            //extract interpreter path
            if (phdr->p_filesz > 0 && phdr->p_filesz < sizeof(info->interp_path)) {
                memcpy(info->interp_path, base + phdr->p_offset, phdr->p_filesz);
                info->interp_path[phdr->p_filesz] = '\0';
            }
        } else if (phdr->p_type == PT_PHDR) {
            info->phdr_addr = phdr->p_vaddr;
        }
    }
    
    //load each segment
    for (uint16 i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(base + ehdr->e_phoff + (i * ehdr->e_phentsize));
        
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        
        //page-align the segment
        uint64 seg_vaddr = phdr->p_vaddr & ~(PAGE_SIZE - 1);
        uint64 seg_offset = phdr->p_vaddr - seg_vaddr;
        uint64 seg_size = (phdr->p_memsz + seg_offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        size seg_pages = seg_size / PAGE_SIZE;
        
        //allocate physical pages
        void *phys = pmm_alloc(seg_pages);
        if (!phys) {
            //rollback already allocated segments
            elf_unload_user(pagemap, info);
            return ELF_ERR_NO_MEMORY;
        }
        
        //zero the pages via HHDM
        void *virt_access = P2V(phys);
        memset(virt_access, 0, seg_size);
        
        //copy file data
        if (phdr->p_filesz > 0) {
            memcpy((uint8 *)virt_access + seg_offset, base + phdr->p_offset, phdr->p_filesz);
        }
        
        //build MMU flags from ELF flags
        uint64 mmu_flags = MMU_FLAG_PRESENT | MMU_FLAG_USER;
        if (phdr->p_flags & PF_W) mmu_flags |= MMU_FLAG_WRITE;
        if (phdr->p_flags & PF_X) mmu_flags |= MMU_FLAG_EXEC;
        
        //unmap before mapping to avoid leaking physical pages if segments overlap or repeat
        vmm_unmap(pagemap, seg_vaddr, seg_pages);
        
        //map into user address space
        vmm_map(pagemap, seg_vaddr, (uintptr)phys, seg_pages, mmu_flags);
        
        //register in VMA list so allocator knows this region is occupied
        process_vma_add(proc, seg_vaddr, seg_size, mmu_flags, NULL, 0);
        
        //track segment for cleanup
        elf_segment_t *seg = &info->segments[info->segment_count++];
        seg->virt_addr = seg_vaddr;
        seg->phys_addr = (uint64)phys;
        seg->pages = seg_pages;
    }
    
    //update vma_next_addr to point past the loaded program
    //this prevents future allocations (like heap VMOs) from colliding with ELF segments
    uintptr elf_end = (max_vaddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (elf_end > proc->vma_next_addr) {
        proc->vma_next_addr = elf_end;
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

void elf_unload_user(pagemap_t *pagemap, elf_load_info_t *info) {
    if (!info) return;
    
    for (uint32 i = 0; i < info->segment_count; i++) {
        elf_segment_t *seg = &info->segments[i];
        
        //unmap from address space
        if (pagemap) {
            vmm_unmap(pagemap, seg->virt_addr, seg->pages);
        }
        
        //free physical pages
        if (seg->phys_addr && seg->pages > 0) {
            pmm_free((void *)seg->phys_addr, seg->pages);
        }
        
        seg->virt_addr = 0;
        seg->phys_addr = 0;
        seg->pages = 0;
    }
    
    info->segment_count = 0;
}

