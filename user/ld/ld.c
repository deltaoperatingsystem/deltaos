//currently it just oads libc.so and applies PLT relocations
 

typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define SYS_EXIT        1
#define SYS_DEBUG_WRITE 3
#define SYS_GET_OBJ     5
#define SYS_HANDLE_READ 6
#define SYS_HANDLE_CLOSE 32
#define SYS_VMO_CREATE  37
#define SYS_VMO_MAP     41

#define HANDLE_RIGHT_READ 0x01

#define AT_NULL  0
#define AT_PHDR  3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_ENTRY 9

#define PT_LOAD    1
#define PT_DYNAMIC 2

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_JMPREL   23

#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define LD_BUFFER_SIZE   0x8000
#define LD_MAX_PHNUM     32

typedef struct { 
    uint64_t a_type, a_val; 
} auxv_t;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct { 
    int64_t d_tag; uint64_t d_val; 
} Elf64_Dyn;

typedef struct { 
    uint32_t st_name; 
    uint8_t st_info, st_other; 
    uint16_t st_shndx; 
    uint64_t st_value, st_size; 
} Elf64_Sym;

typedef struct { 
    uint64_t r_offset, r_info; 
    int64_t r_addend; 
} Elf64_Rela;

typedef struct { 
    uint8_t e_ident[16]; 
    uint16_t e_type, e_machine; 
    uint32_t e_version; 
    uint64_t e_entry, e_phoff, e_shoff; 
    uint32_t e_flags; 
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; 
} Elf64_Ehdr;

extern uint64_t __ld_saved_sp;

static inline int64_t syscall1(uint64_t n, uint64_t a1) {
    int64_t r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1):"rcx","r11","memory"); return r;
}
static inline int64_t syscall3(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3):"rcx","r11","memory"); return r;
}
static inline int64_t syscall5(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t r; register uint64_t r10 __asm__("r10")=a4; register uint64_t r8 __asm__("r8")=a5;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8):"rcx","r11","memory"); return r;
}

//write single character
static void outc(char c) {
    syscall3(SYS_DEBUG_WRITE, (uint64_t)&c, 1, 0);
}

//write hex value
static void outx(uint64_t v) {
    for (int i = 15; i >= 0; i--) {
        int d = (v >> (i * 4)) & 0xF;
        outc((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}

//write newline
static void outn(void) { outc('\n'); }

//write string
static void outs(const char *s) {
    while (*s) outc(*s++);
}

static void die(void) {
    syscall1(SYS_EXIT, 255);
    for (;;) {}  //should never reach
}

static void die_msg(const char *msg) {
    outs("ld.so: ");
    outs(msg);
    outn();
    die();
}

static int seq(const char *a, const char *b) {
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

static uint64_t sym_lookup(const char *name, uint64_t base, Elf64_Sym *sym, char *str, uint32_t *hash) {
    if (!hash || hash[0] == 0 || hash[1] == 0) return 0;  //validate hash table
    uint32_t nb = hash[0];
    uint32_t *bkt = &hash[2];
    uint32_t *chn = &hash[2 + nb];
    uint32_t h = elfhash(name);
    for (uint32_t i = bkt[h % nb]; i; i = chn[i]) {
        if (seq(name, str + sym[i].st_name) && sym[i].st_value) {
            return base + sym[i].st_value;
        }
    }
    return 0;
}

uint64_t ld_main(uint64_t *sp) {
    //parse auxv
    int i = 1;
    while (sp[i]) i++;  i++;
    while (sp[i]) i++;  i++;
    auxv_t *av = (auxv_t *)&sp[i];
    
    uint64_t entry = 0, phdr = 0;
    uint16_t phnum = 0, phent = 0;
    for (i = 0; av[i].a_type; i++) {
        if (av[i].a_type == AT_ENTRY) entry = av[i].a_val;
        else if (av[i].a_type == AT_PHDR) phdr = av[i].a_val;
        else if (av[i].a_type == AT_PHNUM) phnum = av[i].a_val;
        else if (av[i].a_type == AT_PHENT) phent = av[i].a_val;
    }
    
    //find PT_DYNAMIC
    Elf64_Dyn *dyn = 0;
    for (i = 0; i < phnum; i++) {
        Elf64_Phdr *p = (Elf64_Phdr *)(phdr + i * phent);
        if (p->p_type == PT_DYNAMIC) { dyn = (Elf64_Dyn *)p->p_vaddr; break; }
    }
    if (!dyn) return entry;
    
    //parse dynamic
    char *strtab = 0;
    Elf64_Sym *symtab = 0;
    Elf64_Rela *jmprel = 0;
    uint64_t pltsz = 0;
    
    for (Elf64_Dyn *d = dyn; d->d_tag; d++) {
        if (d->d_tag == DT_STRTAB) strtab = (char *)d->d_val;
        else if (d->d_tag == DT_SYMTAB) symtab = (Elf64_Sym *)d->d_val;
        else if (d->d_tag == DT_JMPREL) jmprel = (Elf64_Rela *)d->d_val;
        else if (d->d_tag == DT_PLTRELSZ) pltsz = d->d_val;
    }
    
    if (!strtab || !jmprel) {
        return entry;
    }
    
    //find NEEDED
    //TODO:support multiple DT_NEEDED entries (dependency walk loop)
    const char *needed = 0;
    for (Elf64_Dyn *d = dyn; d->d_tag; d++) {
        if (d->d_tag == DT_NEEDED && strtab) { needed = strtab + d->d_val; break; }
    }
    
    if (!needed) return entry;
    
    //build path on stack
    const char *prefix = "$files/initrd/system/libraries/";
    char path[64];
    int pi = 0;
    while (*prefix) path[pi++] = *prefix++;
    while (*needed && pi < (int)sizeof(path) - 1) path[pi++] = *needed++;
    if (pi >= (int)sizeof(path) - 1) die_msg("library path too long");
    path[pi] = 0;
    
    //open file
    int64_t fh = syscall3(SYS_GET_OBJ, (uint64_t)-1, (uint64_t)path, HANDLE_RIGHT_READ);
    if (fh < 0) { outs("ld.so: failed to open library "); outs(needed - pi - 1); outn(); die(); }
    
    //allocate buffer via VMO (let kernel pick address)
    uint64_t buf_size = LD_BUFFER_SIZE;
    int64_t buf_vmo = syscall3(SYS_VMO_CREATE, buf_size, 0, 0x3F);
    if (buf_vmo < 0) die_msg("failed to create buffer VMO");
    int64_t buf_addr = syscall5(SYS_VMO_MAP, buf_vmo, 0, 0, buf_size, 0x3);
    if (buf_addr < 0) die_msg("failed to map buffer VMO");
    
    int64_t rd = syscall3(SYS_HANDLE_READ, fh, buf_addr, buf_size);
    syscall1(SYS_HANDLE_CLOSE, fh);
    if (rd <= 0) die_msg("failed to read library");
    
    //validate ELF header
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf_addr;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || 
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') die_msg("invalid ELF magic");
    if (eh->e_phnum > LD_MAX_PHNUM) die_msg("too many program headers");
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) die_msg("invalid phentsize");
    
    Elf64_Phdr *ph = (Elf64_Phdr *)(buf_addr + eh->e_phoff);
    uint64_t minv = ~0ULL, maxv = 0;
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
            if (ph[i].p_vaddr < minv) minv = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > maxv) maxv = ph[i].p_vaddr + ph[i].p_memsz;
        }
    }
    uint64_t libsz = ((maxv - minv) + 0xFFF) & ~0xFFFULL;
    
    //map library (let kernel pick address)
    int64_t lib_vmo = syscall3(SYS_VMO_CREATE, libsz, 0, 0x3F);
    if (lib_vmo < 0) die_msg("failed to create library VMO");
    int64_t libbase = syscall5(SYS_VMO_MAP, lib_vmo, 0, 0, libsz, 0x7);
    if (libbase < 0) die_msg("failed to map library VMO");
    
    //copy segments
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_filesz) {
            uint8_t *dst = (uint8_t *)(libbase + ph[i].p_vaddr);
            uint8_t *src = (uint8_t *)(buf_addr + ph[i].p_offset);
            for (uint64_t j = 0; j < ph[i].p_filesz; j++) dst[j] = src[j];
            for (uint64_t j = ph[i].p_filesz; j < ph[i].p_memsz; j++) dst[j] = 0;
        }
    }
    
    //find lib dynamic
    Elf64_Dyn *libdyn = 0;
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { libdyn = (Elf64_Dyn *)(libbase + ph[i].p_vaddr); break; }
    }
    
    //parse lib symbols
    char *libstr = 0;
    Elf64_Sym *libsym = 0;
    uint32_t *libhash = 0;
    for (Elf64_Dyn *d = libdyn; d && d->d_tag; d++) {
        if (d->d_tag == DT_STRTAB) libstr = (char *)(libbase + d->d_val);
        else if (d->d_tag == DT_SYMTAB) libsym = (Elf64_Sym *)(libbase + d->d_val);
        else if (d->d_tag == DT_HASH) libhash = (uint32_t *)(libbase + d->d_val);
    }
    
    if (!libsym || !libstr || !libhash) die_msg("missing symbol tables in library");
    
    //apply relocations to executable (eager binding)
    //TODO: lazy binding via PLT[0] resolver
    uint64_t nr = pltsz / 24;
    
    for (uint64_t j = 0; j < nr; j++) {
        Elf64_Rela *r = &jmprel[j];
        uint32_t typ = r->r_info & 0xFFFFFFFF;
        uint32_t sidx = r->r_info >> 32;
        
        if (typ == R_X86_64_JUMP_SLOT) {
            const char *sn = strtab + symtab[sidx].st_name;
            uint64_t sa = sym_lookup(sn, libbase, libsym, libstr, libhash);
            if (sa) {
                *(uint64_t *)r->r_offset = sa;
            }
        }
    }
    
    //parse lib dynamic for RELA
    Elf64_Rela *lib_jmprel = 0;
    uint64_t lib_pltsz = 0;
    Elf64_Rela *lib_rela = 0;
    uint64_t lib_relasz = 0;
    
    for (Elf64_Dyn *d = libdyn; d && d->d_tag; d++) {
        if (d->d_tag == DT_JMPREL) lib_jmprel = (Elf64_Rela *)(libbase + d->d_val);
        else if (d->d_tag == DT_PLTRELSZ) lib_pltsz = d->d_val;
        else if (d->d_tag == DT_RELA) lib_rela = (Elf64_Rela *)(libbase + d->d_val);
        else if (d->d_tag == DT_RELASZ) lib_relasz = d->d_val;
    }
    
    //apply internal RELA relocations to libc.so
    if (lib_rela) {
        uint64_t nr_rela = lib_relasz / 24;
        for (uint64_t j = 0; j < nr_rela; j++) {
            Elf64_Rela *r = &lib_rela[j];
            uint32_t typ = r->r_info & 0xFFFFFFFF;
            uint32_t sidx = r->r_info >> 32;
            uint64_t *ptr = (uint64_t *)(libbase + r->r_offset);
            
            if (typ == R_X86_64_GLOB_DAT || typ == R_X86_64_64) {
                const char *sn = libstr + libsym[sidx].st_name;
                uint64_t sa = sym_lookup(sn, libbase, libsym, libstr, libhash);
                if (sa) *ptr = sa + r->r_addend;
            } else if (typ == R_X86_64_RELATIVE) {
                *ptr = libbase + r->r_addend;
            }
        }
    }
    
    //apply internal PLT relocations to libc.so
    if (lib_jmprel) {
        uint64_t nlr = lib_pltsz / 24;
        for (uint64_t j = 0; j < nlr; j++) {
            Elf64_Rela *r = &lib_jmprel[j];
            uint32_t typ = r->r_info & 0xFFFFFFFF;
            uint32_t sidx = r->r_info >> 32;
            if (typ == R_X86_64_JUMP_SLOT) {
                const char *sn = libstr + libsym[sidx].st_name;
                uint64_t sa = sym_lookup(sn, libbase, libsym, libstr, libhash);
                if (sa) {
                    *(uint64_t *)(libbase + r->r_offset) = sa;
                }
            }
        }
    }
    
    return entry;
}
