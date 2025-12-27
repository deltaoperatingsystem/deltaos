#ifndef KERNEL_ELF64_H
#define KERNEL_ELF64_H

#include <arch/types.h>

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
#define ET_DYN      3   //shared object (PIE)
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
    uint8       e_ident[16];    //magic and identification
    uint16      e_type;         //object file type
    uint16      e_machine;      //machine type
    uint32      e_version;      //object file version
    uint64      e_entry;        //entry point address
    uint64      e_phoff;        //program header offset
    uint64      e_shoff;        //section header offset
    uint32      e_flags;        //processor-specific flags
    uint16      e_ehsize;       //ELF header size
    uint16      e_phentsize;    //program header entry size
    uint16      e_phnum;        //program header count
    uint16      e_shentsize;    //section header entry size
    uint16      e_shnum;        //section header count
    uint16      e_shstrndx;     //section name string table index
} Elf64_Ehdr;

//ELF64 program header
typedef struct {
    uint32      p_type;         //segment type
    uint32      p_flags;        //segment flags
    uint64      p_offset;       //offset in file
    uint64      p_vaddr;        //virtual address
    uint64      p_paddr;        //physical address
    uint64      p_filesz;       //size in file
    uint64      p_memsz;        //size in memory
    uint64      p_align;        //alignment
} Elf64_Phdr;

//e_ident indices
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6

//load result info
typedef struct {
    uint64 phys_base;       //physical base of loaded image
    uint64 phys_size;       //total size in bytes
    uint64 virt_base;       //virtual base address
    uint64 entry;           //entry point (physical or virtual depending on flags)
    size   pages;           //number of pages allocated
} elf_load_info_t;

//error codes
#define ELF_OK              0
#define ELF_ERR_INVALID     1   //invalid ELF file
#define ELF_ERR_UNSUPPORTED 2   //unsupported format (not 64-bit, wrong arch)
#define ELF_ERR_NO_MEMORY   3   //failed to allocate memory
#define ELF_ERR_NO_SEGMENTS 4   //no loadable segments

//validate an ELF64 file
//returns 1 if valid, 0 if invalid
int elf_validate(const void *data, size len);

//load an ELF64 executable into memory
//allocates physical pages and copies segments
//returns entry point (physical address for flat binaries, virtual for higher-half)
int elf_load(const void *data, size len, elf_load_info_t *info);

//free memory from a loaded ELF
void elf_unload(elf_load_info_t *info);

#endif
