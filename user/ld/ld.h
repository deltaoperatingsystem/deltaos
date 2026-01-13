#ifndef LD_H
#define LD_H

typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

//syscall numbers
#define SYS_EXIT        0
#define SYS_DEBUG_WRITE 3
#define SYS_GET_OBJ     5
#define SYS_HANDLE_READ 6
#define SYS_HANDLE_CLOSE 32
#define SYS_VMO_CREATE  37
#define SYS_VMO_MAP     41
#define SYS_STAT        44

#define HANDLE_RIGHT_READ 0x01

#define FS_TYPE_FILE    1
#define FS_TYPE_DIR     2

typedef struct stat {
    uint32_t type;
    uint64_t size;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
} stat_t;

//aux vector types
#define AT_NULL  0
#define AT_PHDR  3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_ENTRY 9

//program header types
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_PHDR    6

//dynamic tags
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_INIT     12
#define DT_FINI     13
#define DT_JMPREL   23
#define DT_INIT_ARRAY    25
#define DT_FINI_ARRAY    26
#define DT_INIT_ARRAYSZ  27
#define DT_FINI_ARRAYSZ  28
#define DT_RUNPATH       29
#define DT_RPATH         15
#define DT_GNU_HASH      0x6ffffef5

//relocation types
#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

//sanity check limits (for malformed ELF detection)
#define LD_MAX_PHNUM     64  //no real ELF has more than this

//error codes
#define LD_OK            0
#define LD_ERR_NO_AUXV   (-1)
#define LD_ERR_NO_DYN    (-2)
#define LD_ERR_OPEN      (-3)
#define LD_ERR_VMO       (-4)
#define LD_ERR_READ      (-5)
#define LD_ERR_ELF       (-6)
#define LD_ERR_NO_SYM    (-7)
#define LD_ERR_RELOC     (-8)

//ELF structures
typedef struct { uint64_t a_type, a_val; } auxv_t;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct { int64_t d_tag; uint64_t d_val; } Elf64_Dyn;

typedef struct { 
    uint32_t st_name; 
    uint8_t st_info, st_other; 
    uint16_t st_shndx; 
    uint64_t st_value, st_size; 
} Elf64_Sym;

typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;

typedef struct { 
    uint8_t e_ident[16]; 
    uint16_t e_type, e_machine; 
    uint32_t e_version; 
    uint64_t e_entry, e_phoff, e_shoff; 
    uint32_t e_flags; 
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; 
} Elf64_Ehdr;

//parsed executable info from auxv
typedef struct {
    uint64_t entry;      //AT_ENTRY - program entry point
    uint64_t phdr;       //AT_PHDR - program header address
    uint16_t phnum;      //AT_PHNUM - number of program headers
    uint16_t phent;      //AT_PHENT - size of program header
    Elf64_Dyn *dynamic;  //pointer to PT_DYNAMIC
    char *strtab;        //DT_STRTAB
    Elf64_Sym *symtab;   //DT_SYMTAB
    Elf64_Rela *jmprel;  //DT_JMPREL
    uint64_t pltrelsz;   //DT_PLTRELSZ
    uint64_t *pltgot;    //DT_PLTGOT
    Elf64_Rela *rela;    //DT_RELA
    uint64_t relasz;     //DT_RELASZ
    uint32_t *hashtab;   //DT_HASH
    uint32_t *gnu_hashtab; //DT_GNU_HASH (preferred)
    char *runpath;       //DT_RUNPATH or DT_RPATH
    void (**init_array)(void);
    uint64_t init_arraysz;
} elf_info_t;

//loaded library handle
typedef struct {
    char *name;             //library name (dynamically allocated)
    uint64_t base;          //load base address
    uint64_t size;          //mapped size (for bounds checking)
    Elf64_Dyn *dynamic;     //PT_DYNAMIC pointer
    Elf64_Sym *symtab;      //symbol table
    char *strtab;           //string table
    uint32_t *hashtab;      //ELF hash table
    uint32_t symtab_count;  //number of symbols (from hash nchain)
    uint32_t strtab_size;   //string table size (for bounds check)
    Elf64_Rela *rela;       //RELA relocations
    uint64_t relasz;        //RELA size
    Elf64_Rela *jmprel;     //PLT relocations
    uint64_t pltrelsz;      //PLT size
    //init/fini arrays
    void (**init_array)(void);  //DT_INIT_ARRAY
    uint64_t init_arraysz;      //DT_INIT_ARRAYSZ
    void (**fini_array)(void);  //DT_FINI_ARRAY
    uint64_t fini_arraysz;      //DT_FINI_ARRAYSZ
    void (*init_func)(void);    //DT_INIT (legacy)
    void (*fini_func)(void);    //DT_FINI (legacy)
    uint64_t *pltgot;           //DT_PLTGOT
    uint32_t *gnu_hashtab;      //DT_GNU_HASH (preferred, faster)
} lib_handle_t;

#endif
