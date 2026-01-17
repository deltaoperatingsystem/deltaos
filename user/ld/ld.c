#include "ld.h"

extern uint64_t __ld_saved_sp;
extern void _ld_runtime_resolve_asm(void);

static inline int64_t syscall1(uint64_t n, uint64_t a1) {
    int64_t r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1):"rcx","r11","memory");
    return r;
}
static inline int64_t syscall2(uint64_t n, uint64_t a1, uint64_t a2) {
    int64_t r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2):"rcx","r11","memory");
    return r;
}

static inline int64_t syscall3(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3):"rcx","r11","memory");
    return r;
}

static inline int64_t syscall5(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t r;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8 __asm__("r8") = a5;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8):"rcx","r11","memory");
    return r;
}

static void outc(char c) {
    syscall3(SYS_DEBUG_WRITE, (uint64_t)&c, 1, 0);
}

static void outs(const char *s) {
    while (*s) outc(*s++);
}

static void outn(void) {
    outc('\n'); 
}

static void outx(uint64_t v) {
    for (int i = 15; i >= 0; i--) {
        int d = (v >> (i * 4)) & 0xF;
        outc((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}

static void die(void) {
    syscall1(SYS_EXIT, 255);
    for (;;) {}
}

static void die_msg(const char *msg) {
    outs("ld.so: ");
    outs(msg);
    outn();
    die();
}

static void die_err(int err) {
    outs("ld.so: error ");
    outc('0' + (-err));
    outn();
    die();
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint32_t elfhash(const char *n) {
    uint32_t h = 0;
    for (; *n; n++) {
        h = (h << 4) + *n;
        uint32_t g = h & 0xF0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

//GNU hash function (djb2 variant)
static uint32_t gnu_hash(const char *n) {
    uint32_t h = 5381;
    for (; *n; n++) h = (h << 5) + h + (uint8_t)*n;
    return h;
}

//growable bump allocator - allocates new pages as needed
#define LD_HEAP_CHUNK 0x10000  //64KB per chunk

static uint8_t *ld_heap_ptr = 0;
static uint8_t *ld_heap_end = 0;

static int ld_heap_grow(uint64_t min_size) {
    //allocate at least one chunk or more if needed
    uint64_t alloc_size = LD_HEAP_CHUNK;
    if (min_size > alloc_size) {
        alloc_size = (min_size + 0xFFF) & ~0xFFFULL;  //page align
    }
    
    int64_t vmo = syscall3(SYS_VMO_CREATE, alloc_size, 0, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_MAP);
    if (vmo < 0) return -1;
    
    int64_t addr = syscall5(SYS_VMO_MAP, vmo, 0, 0, alloc_size, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    if (addr <= 0) return -1;
    
    ld_heap_ptr = (uint8_t *)addr;
    ld_heap_end = ld_heap_ptr + alloc_size;
    return 0;
}

static void *ld_alloc(uint64_t size) {
    //align to 8 bytes
    size = (size + 7) & ~7ULL;
    
    //grow heap if needed
    if (!ld_heap_ptr || ld_heap_ptr + size > ld_heap_end) {
        if (ld_heap_grow(size) < 0) return 0;
    }
    
    void *p = ld_heap_ptr;
    ld_heap_ptr += size;
    return p;
}

//library list
typedef struct lib_node {
    lib_handle_t lib;
    struct lib_node *next;
} lib_node_t;

static lib_node_t *g_lib_list = 0;
static int g_lib_count = 0;
static lib_node_t g_exe_node; //persistent node for main exe

//library search paths
static const char *ld_default_paths[] = {
    "$files/initrd/system/libraries/",
    "$files/initrd/lib/",
    0
};
static char *ld_runpath = 0;  //from DT_RUNPATH or DT_RPATH

//copy string
static void ld_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

//check if library is already loaded
static lib_handle_t *ld_find_lib(const char *name) {
    for (lib_node_t *n = g_lib_list; n; n = n->next) {
        if (streq(n->lib.name, name)) {
            return &n->lib;
        }
    }
    return 0;
}

//add a library to the list
static lib_handle_t *ld_add_lib(void) {
    lib_node_t *node = (lib_node_t *)ld_alloc(sizeof(lib_node_t));
    if (!node) return 0;
    
    node->next = 0;
    
    //add to end of list
    if (!g_lib_list) {
        g_lib_list = node;
    } else {
        lib_node_t *tail = g_lib_list;
        while (tail->next) tail = tail->next;
        tail->next = node;
    }
    g_lib_count++;
    return &node->lib;
}

//parse auxv
static int elf_parse_auxv(uint64_t *sp, elf_info_t *info) {
    //skip argc
    int i = 1;
    //skip argv
    while (sp[i]) i++;
    i++;
    //skip envp
    while (sp[i]) i++;
    i++;
    
    //parse auxv
    auxv_t *av = (auxv_t *)&sp[i];
    info->entry = 0;
    info->phdr = 0;
    info->phnum = 0;
    info->phent = 0;
    
    for (i = 0; av[i].a_type != AT_NULL; i++) {
        switch (av[i].a_type) {
            case AT_ENTRY: info->entry = av[i].a_val; break;
            case AT_PHDR:  info->phdr = av[i].a_val; break;
            case AT_PHNUM: info->phnum = (uint16_t)av[i].a_val; break;
            case AT_PHENT: info->phent = (uint16_t)av[i].a_val; break;
        }
    }
    
    if (!info->phdr || !info->phnum) return LD_ERR_NO_AUXV;
    return LD_OK;
}

//find PT_DYNAMIC and parse it
static int elf_find_dynamic(elf_info_t *info) {
    info->dynamic = 0;
    info->strtab = 0;
    info->symtab = 0;
    info->jmprel = 0;
    info->pltrelsz = 0;
    info->pltgot = 0;
    info->gnu_hashtab = 0;
    info->runpath = 0;
    
    //find PT_DYNAMIC
    for (int i = 0; i < info->phnum; i++) {
        Elf64_Phdr *p = (Elf64_Phdr *)(info->phdr + i * info->phent);
        if (p->p_type == PT_DYNAMIC) {
            info->dynamic = (Elf64_Dyn *)p->p_vaddr;
            break;
        }
    }
    
    if (!info->dynamic) return LD_ERR_NO_DYN;
    
    //parse dynamic section
    for (Elf64_Dyn *d = info->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB:   info->strtab = (char *)d->d_val; break;
            case DT_SYMTAB:   info->symtab = (Elf64_Sym *)d->d_val; break;
            case DT_JMPREL:   info->jmprel = (Elf64_Rela *)d->d_val; break;
            case DT_PLTRELSZ: info->pltrelsz = d->d_val; break;
            case DT_PLTGOT:   info->pltgot = (uint64_t *)d->d_val; break;
            case DT_RELA:     info->rela = (Elf64_Rela *)d->d_val; break;
            case DT_RELASZ:   info->relasz = d->d_val; break;
            case DT_HASH:     info->hashtab = (uint32_t *)d->d_val; break;
            case DT_GNU_HASH: info->gnu_hashtab = (uint32_t *)d->d_val; break;
            case DT_RUNPATH:  info->runpath = (char *)d->d_val; break;  //preferred
            case DT_RPATH:    if (!info->runpath) info->runpath = (char *)d->d_val; break;
            case DT_INIT_ARRAY:   info->init_array = (void (**)(void))d->d_val; break;
            case DT_INIT_ARRAYSZ: info->init_arraysz = d->d_val; break;
        }
    }
    
    return LD_OK;
}

//load a library (forward declaration for recursion)
static int ld_load_library(const char *name, lib_handle_t *lib);

//load a library and its dependencies recursively
static int ld_load_library(const char *name, lib_handle_t *lib) {
    //calculate name length
    int name_len = 0;
    while (name[name_len]) name_len++;
    
    //dynamically allocate name
    lib->name = (char *)ld_alloc(name_len + 1);
    if (!lib->name) return LD_ERR_VMO;
    ld_strcpy(lib->name, name, name_len + 1);
    
    //try to find library in search paths
    char path[128];
    stat_t st;
    int found = 0;
    
    //build path helper
    #define TRY_PATH(prefix) do { \
        int pi = 0; \
        const char *p = (prefix); \
        while (*p && pi < (int)sizeof(path) - 1) path[pi++] = *p++; \
        const char *n = name; \
        while (*n && pi < (int)sizeof(path) - 1) path[pi++] = *n++; \
        if (*n == '\0') { \
            path[pi] = 0; \
            if (syscall2(SYS_STAT, (uint64_t)path, (uint64_t)&st) >= 0 && st.type == FS_TYPE_FILE) { \
                found = 1; \
            } \
        } \
    } while(0)
    
    //try runpath first (from executable's DT_RUNPATH/DT_RPATH)
    if (!found && ld_runpath) {
        TRY_PATH(ld_runpath);
    }
    
    //try default paths
    for (int i = 0; !found && ld_default_paths[i]; i++) {
        TRY_PATH(ld_default_paths[i]);
    }
    
    #undef TRY_PATH
    
    if (!found) {
        outs("ld.so: could not find library: ");
        outs(name);
        outn();
        return LD_ERR_OPEN;
    }
    
/*
    outs("ld.so: loading: ");
    outs(path);
    outn();
*/
    
    //allocate exact read buffer via bump allocator
    uint64_t buf_size = st.size;
    uint8_t *buf = (uint8_t *)ld_alloc(buf_size);
    if (!buf) return LD_ERR_VMO;
    
    //open file
    int64_t fh = syscall3(SYS_GET_OBJ, (uint64_t)-1, (uint64_t)path, HANDLE_RIGHT_READ);
    if (fh < 0) return LD_ERR_OPEN;
    
    //read library
    int64_t rd = syscall3(SYS_HANDLE_READ, fh, (uint64_t)buf, buf_size);
    syscall1(SYS_HANDLE_CLOSE, fh);
    if (rd < (int64_t)buf_size) return LD_ERR_READ;
    
    //validate ELF header
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return LD_ERR_ELF;
    if (eh->e_phnum > LD_MAX_PHNUM) return LD_ERR_ELF;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return LD_ERR_ELF;
    
    //calculate library size from segments
    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff);
    uint64_t minv = ~0ULL, maxv = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
            if (ph[i].p_vaddr < minv) minv = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > maxv) maxv = ph[i].p_vaddr + ph[i].p_memsz;
        }
    }
    uint64_t libsz = ((maxv - minv) + 0xFFF) & ~0xFFFULL;
    
    //map library VMO
    int64_t lib_vmo = syscall3(SYS_VMO_CREATE, libsz, 0, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_EXECUTE | HANDLE_RIGHT_MAP);
    if (lib_vmo < 0) return LD_ERR_VMO;
    
    int64_t base = syscall5(SYS_VMO_MAP, lib_vmo, 0, 0, libsz, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_EXECUTE);
    if (base < 0) return LD_ERR_VMO;
    
    lib->base = (uint64_t)base;
    
    //copy segments
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_filesz) {
            uint8_t *dst = (uint8_t *)(lib->base + ph[i].p_vaddr);
            uint8_t *src = buf + ph[i].p_offset;
            for (uint64_t j = 0; j < ph[i].p_filesz; j++) dst[j] = src[j];
            for (uint64_t j = ph[i].p_filesz; j < ph[i].p_memsz; j++) dst[j] = 0;
        }
    }
    
    //find PT_DYNAMIC in library
    Elf64_Dyn *libdyn = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            libdyn = (Elf64_Dyn *)(lib->base + ph[i].p_vaddr);
            break;
        }
    }
    
    //parse library dynamic section
    lib->symtab = 0;
    lib->strtab = 0;
    lib->hashtab = 0;
    lib->rela = 0;
    lib->relasz = 0;
    lib->jmprel = 0;
    lib->pltrelsz = 0;
    lib->pltgot = 0;
    lib->symtab_count = 0;
    lib->strtab_size = 0;
    lib->init_array = 0;
    lib->init_arraysz = 0;
    lib->fini_array = 0;
    lib->fini_arraysz = 0;
    lib->init_func = 0;
    lib->fini_func = 0;
    lib->gnu_hashtab = 0;
    
    for (Elf64_Dyn *d = libdyn; d && d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB:   lib->strtab = (char *)(lib->base + d->d_val); break;
            case DT_SYMTAB:   lib->symtab = (Elf64_Sym *)(lib->base + d->d_val); break;
            case DT_HASH:     lib->hashtab = (uint32_t *)(lib->base + d->d_val); break;
            case DT_RELA:     lib->rela = (Elf64_Rela *)(lib->base + d->d_val); break;
            case DT_RELASZ:   lib->relasz = d->d_val; break;
            case DT_JMPREL:   lib->jmprel = (Elf64_Rela *)(lib->base + d->d_val); break;
            case DT_PLTRELSZ: lib->pltrelsz = d->d_val; break;
            case DT_PLTGOT:   lib->pltgot = (uint64_t *)(lib->base + d->d_val); break;
            case DT_INIT:     lib->init_func = (void (*)(void))(lib->base + d->d_val); break;
            case DT_FINI:     lib->fini_func = (void (*)(void))(lib->base + d->d_val); break;
            case DT_INIT_ARRAY:   lib->init_array = (void (**)(void))(lib->base + d->d_val); break;
            case DT_INIT_ARRAYSZ: lib->init_arraysz = d->d_val; break;
            case DT_FINI_ARRAY:   lib->fini_array = (void (**)(void))(lib->base + d->d_val); break;
            case DT_FINI_ARRAYSZ: lib->fini_arraysz = d->d_val; break;
            case DT_GNU_HASH:     lib->gnu_hashtab = (uint32_t *)(lib->base + d->d_val); break;
        }
    }
    
    //extract symbol count from hash table (nchain = hash[1])
    if (lib->hashtab) {
        uint32_t nchain = lib->hashtab[1];
        //sanity check: nchain shouldn't be absurdly large
        if (nchain > 0x100000) return LD_ERR_ELF;  //1M symbols max
        lib->symtab_count = nchain;
    }
    
    //need either ELF hash or GNU hash
    if (!lib->symtab || !lib->strtab || (!lib->hashtab && !lib->gnu_hashtab)) return LD_ERR_NO_SYM;
    
    //store library size for relocation bounds checking
    lib->size = libsz;
    
    //store dynamic pointer for later use
    lib->dynamic = libdyn;
    
    //recursively load dependencies (DT_NEEDED)
    if (libdyn && lib->strtab) {
        for (Elf64_Dyn *d = libdyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED) {
                const char *dep_name = lib->strtab + d->d_val;
                
                //check if already loaded
                if (ld_find_lib(dep_name)) continue;
                
                //allocate new library entry
                lib_handle_t *dep = ld_add_lib();
                if (!dep) return LD_ERR_VMO;  //out of memory
                
                int err = ld_load_library(dep_name, dep);
                if (err < 0) return err;
            }
        }
    }
    
    return LD_OK;
}

//GNU hash symbol lookup (faster than ELF hash)
static uint64_t ld_gnu_sym_lookup(const char *name, lib_handle_t *lib) {
    uint32_t *gnu = lib->gnu_hashtab;
    if (!gnu) return 0;
    
    //parse GNU hash header
    uint32_t nbuckets = gnu[0];
    uint32_t symoffset = gnu[1];  //first symbol index in chain
    uint32_t bloom_size = gnu[2];
    uint32_t bloom_shift = gnu[3];
    
    if (nbuckets == 0) return 0;
    
    uint32_t h = gnu_hash(name);
    
    //bloom filter check (quick rejection)
    uint64_t *bloom = (uint64_t *)&gnu[4];
    uint64_t mask = ((uint64_t)1 << (h & 63)) | ((uint64_t)1 << ((h >> bloom_shift) & 63));
    uint64_t word = bloom[(h / 64) % bloom_size];
    if ((word & mask) != mask) return 0;  //definitely not present
    
    //bucket lookup
    uint32_t *buckets = (uint32_t *)&bloom[bloom_size];
    uint32_t *chains = &buckets[nbuckets];
    
    uint32_t idx = buckets[h % nbuckets];
    if (idx == 0) return 0;  //empty bucket
    
    //walk the chain
    uint32_t h1 = h | 1;  //set LSB for comparison (chains store hash with LSB as end marker)
    for (;;) {
        uint32_t chain_idx = idx - symoffset;
        uint32_t chain_hash = chains[chain_idx];
        
        //compare hash (ignore LSB which is end-of-chain marker)
        if ((chain_hash | 1) == h1) {
            //hash matches, verify name
            Elf64_Sym *sym = &lib->symtab[idx];
            if (streq(name, lib->strtab + sym->st_name) && sym->st_shndx != 0) {
                return lib->base + sym->st_value;
            }
        }
        
        //check end of chain (LSB set)
        if (chain_hash & 1) break;
        idx++;
    }
    return 0;
}

//symbol lookup (prefers GNU hash and falls back to ELF hash)
static uint64_t ld_sym_lookup(const char *name, lib_handle_t *lib) {
    //try GNU hash first (faster)
    if (lib->gnu_hashtab) {
        return ld_gnu_sym_lookup(name, lib);
    }
    
    //fall back to ELF hash
    if (!lib->hashtab || lib->hashtab[0] == 0) return 0;
    
    uint32_t nbucket = lib->hashtab[0];
    uint32_t nchain = lib->hashtab[1];
    uint32_t *buckets = &lib->hashtab[2];
    uint32_t *chains = &lib->hashtab[2 + nbucket];
    
    uint32_t h = elfhash(name);
    
    for (uint32_t i = buckets[h % nbucket]; i != 0 && i < nchain; i = chains[i]) {
        //bounds check
        if (i >= lib->symtab_count) return 0;
        
        Elf64_Sym *sym = &lib->symtab[i];
        if (streq(name, lib->strtab + sym->st_name) && sym->st_shndx != 0) {
            return lib->base + sym->st_value;
        }
    }
    return 0;
}

//lookup symbol in all loaded objects (executable first, then libraries)
static uint64_t ld_sym_glob_lookup(const char *name) {
    for (lib_node_t *n = g_lib_list; n; n = n->next) {
        uint64_t addr = ld_sym_lookup(name, &n->lib);
        if (addr) return addr;
    }
    return 0;
}

//C handler for lazy binding
//csalled by _ld_runtime_resolve_asm
uint64_t ld_runtime_resolve(lib_handle_t *lib, uint64_t reloc_index) {
    if (!lib) die_msg("lazy resolution failed: lib is NULL");
    
    //find the relocation entry
    if (!lib->jmprel) die_msg("lazy resolution failed: no jmprel");
    Elf64_Rela *reloc = &lib->jmprel[reloc_index];
    
    uint32_t sidx = reloc->r_info >> 32;
    if (sidx >= lib->symtab_count) die_msg("lazy resolution failed: invalid symbol index");
    
    const char *name = lib->strtab + lib->symtab[sidx].st_name;
    
    //lookup symbol globally
    uint64_t addr = ld_sym_glob_lookup(name);
    if (!addr) {
        outs("ld.so: lazy resolution failed for symbol: ");
        outs(name);
        outn();
        die();
    }
    
    //update GOT entry
    *(uint64_t *)(lib->base + reloc->r_offset) = addr;
    
    return addr;
}

//apply relocations
static int ld_apply_relocs(Elf64_Rela *relocs, uint64_t size, 
                           Elf64_Sym *symtab, char *strtab, 
                           uint64_t target_base, int lazy) {
    uint64_t count = size / sizeof(Elf64_Rela);
    
    for (uint64_t i = 0; i < count; i++) {
        Elf64_Rela *r = &relocs[i];
        uint32_t type = r->r_info & 0xFFFFFFFF;
        uint32_t sidx = r->r_info >> 32;
        
        if (type == R_X86_64_JUMP_SLOT) {
            if (lazy) continue; //skip for lazy binding
            
            const char *name = strtab + symtab[sidx].st_name;
            uint64_t addr = ld_sym_glob_lookup(name);
            if (!addr) {
                outs("ld.so: undefined symbol: ");
                outs(name);
                outn();
                die();
            }
            *(uint64_t *)(target_base + r->r_offset) = addr;
        }
    }
    return LD_OK;
}

static int ld_apply_lib_relocs(lib_handle_t *lib, int lazy) {
    //apply RELA relocations
    if (lib->rela) {
        uint64_t count = lib->relasz / sizeof(Elf64_Rela);
        for (uint64_t i = 0; i < count; i++) {
            Elf64_Rela *r = &lib->rela[i];
            uint32_t type = r->r_info & 0xFFFFFFFF;
            uint32_t sidx = r->r_info >> 32;
            uint64_t *ptr = (uint64_t *)(lib->base + r->r_offset);
            
            //bounds check symbol index
            if (sidx >= lib->symtab_count && type != R_X86_64_RELATIVE) continue;
            
            if (type == R_X86_64_GLOB_DAT || type == R_X86_64_64) {
                const char *name = lib->strtab + lib->symtab[sidx].st_name;
                uint64_t addr = ld_sym_glob_lookup(name);
                if (!addr) {
                    outs("ld.so: undefined symbol: ");
                    outs(name);
                    outn();
                    die();
                }
                *ptr = addr + r->r_addend;
            } else if (type == R_X86_64_RELATIVE) {
                *ptr = lib->base + r->r_addend;
            }
        }
    }
    
    //apply PLT relocations
    if (lib->jmprel) {
        uint64_t count = lib->pltrelsz / sizeof(Elf64_Rela);
        for (uint64_t i = 0; i < count; i++) {
            Elf64_Rela *r = &lib->jmprel[i];
            uint32_t type = r->r_info & 0xFFFFFFFF;
            uint32_t sidx = r->r_info >> 32;
            
            //bounds check
            if (sidx >= lib->symtab_count) continue;
            
            if (type == R_X86_64_JUMP_SLOT) {
                if (lazy) {
                    //rebase the GOT entry so it points to the PLT stub within the library
                    //initial value is an offset from 0 (or from link address)
                    uint64_t *ptr = (uint64_t *)(lib->base + r->r_offset);
                    *ptr += lib->base;
                    continue; //skip resolution for now
                }
                
                const char *name = lib->strtab + lib->symtab[sidx].st_name;
                uint64_t addr = ld_sym_glob_lookup(name);
                if (!addr) {
                    outs("ld.so: undefined symbol: ");
                    outs(name);
                    outn();
                    die();
                }
                *(uint64_t *)(lib->base + r->r_offset) = addr;
            }
        }
    }
    
    return LD_OK;
}

//call library constructors (init arrays)
static void ld_call_init(lib_handle_t *lib) {
    //call legacy DT_INIT first
    if (lib->init_func) {
        lib->init_func();
    }
    
    //call DT_INIT_ARRAY entries
    if (lib->init_array && lib->init_arraysz > 0) {
        uint64_t count = lib->init_arraysz / sizeof(void *);
        for (uint64_t i = 0; i < count; i++) {
            if (lib->init_array[i]) {
                lib->init_array[i]();
            }
        }
    }
}

//call library destructors (fini arrays) - called at exit
//for now this is not hooked up since we don't have atexit
static void ld_call_fini(lib_handle_t *lib) {
    //call DT_FINI_ARRAY entries in reverse order
    if (lib->fini_array && lib->fini_arraysz > 0) {
        uint64_t count = lib->fini_arraysz / sizeof(void *);
        for (uint64_t i = count; i > 0; i--) {
            if (lib->fini_array[i - 1]) {
                lib->fini_array[i - 1]();
            }
        }
    }
    
    //call legacy DT_FINI last
    if (lib->fini_func) {
        lib->fini_func();
    }
}

//setup lazy binding hooks in GOT
static void ld_setup_lazy_binding(void *lib_ptr, uint64_t *pltgot) {
    if (!pltgot) return;
    
    //GOT[1] = identifier (lib_handle_t* or 0 for exe)
    //GOT[2] = trampoline address
    pltgot[1] = (uint64_t)lib_ptr;
    pltgot[2] = (uint64_t)_ld_runtime_resolve_asm;
}

uint64_t ld_main(uint64_t *sp) {
    elf_info_t info;
    int err;
    
    //zero out info struct (it's on the stack)
    uint8_t *infop = (uint8_t *)&info;
    for (int i = 0; i < sizeof(elf_info_t); i++) infop[i] = 0;
    
    //parse auxv
    err = elf_parse_auxv(sp, &info);
    if (err < 0) return 0;
    
    //calculate executable base address
    uint64_t exe_base = 0;
    if (info.phdr) {
        //find the first LOAD segment or PT_PHDR to determine base
        for (int i = 0; i < info.phnum; i++) {
            Elf64_Phdr *p = (Elf64_Phdr *)(info.phdr + i * info.phent);
            if (p->p_type == PT_PHDR) {
                exe_base = info.phdr - p->p_vaddr;
                break;
            } else if (p->p_type == PT_LOAD && exe_base == 0) {
                //fallback: many PIE binaries have phdr at offset 0x40
                if (p->p_offset == 0) exe_base = info.phdr - 0x40;
            }
        }
    }

    //find dynamic section
    err = elf_find_dynamic(&info);
    if (err < 0) return info.entry + exe_base;
    
    //adjust pointers in info by base if it's a PIE relative address
    if (exe_base) {
        if (info.strtab && (uint64_t)info.strtab < exe_base) info.strtab += exe_base;
        if (info.symtab && (uint64_t)info.symtab < exe_base) info.symtab = (Elf64_Sym *)((uint64_t)info.symtab + exe_base);
        if (info.jmprel && (uint64_t)info.jmprel < exe_base) info.jmprel = (Elf64_Rela *)((uint64_t)info.jmprel + exe_base);
        if (info.pltgot && (uint64_t)info.pltgot < exe_base) info.pltgot = (uint64_t *)((uint64_t)info.pltgot + exe_base);
        if (info.hashtab && (uint64_t)info.hashtab < exe_base) info.hashtab = (uint32_t *)((uint64_t)info.hashtab + exe_base);
        if (info.gnu_hashtab && (uint64_t)info.gnu_hashtab < exe_base) info.gnu_hashtab = (uint32_t *)((uint64_t)info.gnu_hashtab + exe_base);
        if (info.rela && (uint64_t)info.rela < exe_base) info.rela = (Elf64_Rela *)((uint64_t)info.rela + exe_base);
        //note: info.runpath is an offset into strtab, not a pointer so no base adjustment needed
    }
    
    if (!info.strtab) return info.entry + exe_base;
    
    //set global runpath for library loading (runpath is offset into strtab)
    if (info.runpath && info.strtab) {
        ld_runpath = info.strtab + (uint64_t)info.runpath;
    }
    
    //populate persistent exe node
    g_exe_node.lib.name = "init";
    g_exe_node.lib.base = exe_base;
    g_exe_node.lib.dynamic = info.dynamic;
    g_exe_node.lib.symtab = info.symtab;
    g_exe_node.lib.strtab = info.strtab;
    g_exe_node.lib.hashtab = info.hashtab;
    g_exe_node.lib.gnu_hashtab = info.gnu_hashtab;
    g_exe_node.lib.rela = info.rela;
    g_exe_node.lib.relasz = info.relasz;
    g_exe_node.lib.jmprel = info.jmprel;
    g_exe_node.lib.pltrelsz = info.pltrelsz;
    g_exe_node.lib.pltgot = info.pltgot;
    g_exe_node.lib.init_array = info.init_array;
    g_exe_node.lib.init_arraysz = info.init_arraysz;
    g_exe_node.lib.symtab_count = info.hashtab ? info.hashtab[1] : 0;
    g_exe_node.next = 0;
    
    //add EXE as first node in library list
    g_lib_list = &g_exe_node;
    g_lib_count = 1;

    //load dependencies
    for (Elf64_Dyn *d = info.dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            const char *name = info.strtab + d->d_val;
            if (ld_find_lib(name)) continue;
            lib_handle_t *lib = ld_add_lib();
            if (!lib) die_msg("out of memory");
            err = ld_load_library(name, lib);
            if (err < 0) die_err(err);
        }
    }
    
    //setup and apply relocations for ALL objects (EXE + LIBS)
    for (lib_node_t *n = g_lib_list; n; n = n->next) {
        ld_setup_lazy_binding(&n->lib, n->lib.pltgot);
        ld_apply_lib_relocs(&n->lib, 1);  //lazy=1
    }
    
    //call constructors for all objects (dependencies first so load order works)
    for (lib_node_t *n = g_lib_list; n; n = n->next) {
        ld_call_init(&n->lib);
    }
    
    return info.entry + exe_base;
}
