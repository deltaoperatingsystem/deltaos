#ifndef MM_PMM_H
#define MM_PMM_H

#include <arch/types.h>

#define PAGE_SIZE 4096

void pmm_init(void);

void *pmm_alloc(size pages);
void pmm_free(void *ptr, size pages);

size pmm_get_total_pages(void);
size pmm_get_free_pages(void);

#endif