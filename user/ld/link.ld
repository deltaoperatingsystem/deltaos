OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R_X */
    rodata PT_LOAD FLAGS(4); /* R__ */
    data PT_LOAD FLAGS(6);   /* RW_ */
}

SECTIONS
{
    . = 0x100000;  /* load at 1MB */
    
    .text : {
        *(.text)
        *(.text.*)
    } :text
    
    . = ALIGN(4096);  /* page-align rodata */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :rodata
    
    . = ALIGN(4096);  /* page-align data */
    .data : {
        *(.data)
        *(.data.*)
    } :data
    
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :data
    
    .dynamic : {
        *(.dynamic)
    } :data
    
    .hash : {
        *(.hash)
    } :data
    
    .dynsym : {
        *(.dynsym)
    } :data
    
    .dynstr : {
        *(.dynstr)
    } :data
    
    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
