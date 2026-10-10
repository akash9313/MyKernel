#ifndef ASTRA_KERNEL_PMM_H
#define ASTRA_KERNEL_PMM_H

#include <kernel/types.h>
#include <limine.h>

#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12ULL

#define PMM_SUCCESS           0
#define PMM_ERR_NOMEM        -1
#define PMM_ERR_UNALIGNED    -2
#define PMM_ERR_PAGE_ZERO    -3
#define PMM_ERR_UNMANAGED    -4
#define PMM_ERR_DOUBLE_FREE  -5
#define PMM_ERR_INVALID_ARG  -6

struct pmm_stats {
    size_t total_managed_pages; /* Pages across all managed usable regions */
    size_t allocated_pages;     /* Currently allocated pages (including PMM metadata) */
    size_t free_pages;          /* Pages currently available for allocation */
    size_t metadata_pages;      /* Pages reserved for PMM metadata and bitmaps */
    size_t region_count;        /* Number of managed physical memory regions */
};

/* PMM Public API */
int pmm_init(struct limine_memmap_response *mmap, u64 hhdm_offset);
int pmm_alloc_page(phys_addr_t *out_paddr);
int pmm_free_page(phys_addr_t paddr);
void pmm_get_stats(struct pmm_stats *out_stats);
void pmm_print_stats(void);
bool pmm_self_test(u64 hhdm_offset);

#endif /* ASTRA_KERNEL_PMM_H */
