#ifndef _ELF_H
#define _ELF_H

#include <stdint.h>
#include "../boot/uefi/efi.h"

//ELF magic
#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

//ELF class (32/64-bit)
#define ELFCLASSNONE    0
#define ELFCLASS32      1
#define ELFCLASS64      2

//ELF data encoding
#define ELFDATANONE     0
#define ELFDATA2LSB     1   //little-endian
#define ELFDATA2MSB     2   //big-endian

//ELF type
#define ET_NONE     0
#define ET_REL      1   //relocatable
#define ET_EXEC     2   //executable
#define ET_DYN      3   //shared object
#define ET_CORE     4

//machine types
#define EM_X86_64   62  //amd64

//program header types
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_SHLIB    5
#define PT_PHDR     6

//program header flags
#define PF_X        0x1     //execute
#define PF_W        0x2     //write
#define PF_R        0x4     //read

//ELF64 header
typedef struct {
    uint8_t     e_ident[16];    //magic and identification
    uint16_t    e_type;         //object file type
    uint16_t    e_machine;      //machine type
    uint32_t    e_version;      //object file version
    uint64_t    e_entry;        //entry point address
    uint64_t    e_phoff;        //program header offset
    uint64_t    e_shoff;        //section header offset
    uint32_t    e_flags;        //processor-specific flags
    uint16_t    e_ehsize;       //ELF header size
    uint16_t    e_phentsize;    //program header entry size
    uint16_t    e_phnum;        //program header count
    uint16_t    e_shentsize;    //section header entry size
    uint16_t    e_shnum;        //section header count
    uint16_t    e_shstrndx;     //section name string table index
} Elf64_Ehdr;

//ELF64 program header
typedef struct {
    uint32_t    p_type;         //segment type
    uint32_t    p_flags;        //segment flags
    uint64_t    p_offset;       //offset in file
    uint64_t    p_vaddr;        //virtual address
    uint64_t    p_paddr;        //physical address
    uint64_t    p_filesz;       //size in file
    uint64_t    p_memsz;        //size in memory
    uint64_t    p_align;        //alignment
} Elf64_Phdr;

//e_ident indices
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6

//kernel load information
typedef struct {
    uint64_t phys_base;     //lowest physical load address
    uint64_t phys_end;      //highest physical address (exclusive)
    uint64_t virt_base;     //lowest virtual address
    uint64_t virt_entry;    //entry point (virtual)
} elf_load_info_t;

int elf_validate(const void *data, uint64_t size);

//load ELF segments into memory returns entry point and load info
EFI_STATUS elf_load(
    EFI_BOOT_SERVICES *bs,
    const void *elf_data,
    uint64_t elf_size,
    uint64_t *entry_point,
    elf_load_info_t *load_info
);

#endif

