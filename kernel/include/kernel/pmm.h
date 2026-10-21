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
#define PMM_ERR_RESERVED     -7

#define MAX_PMM_REGIONS 64

struct pmm_region {
    phys_addr_t base;
    phys_addr_t end;
    size_t total_pages;
    size_t free_pages;
    size_t last_search_idx;
    u8 *bitmap;
    size_t bitmap_bytes;
};

struct pmm_context {
    struct pmm_region regions[MAX_PMM_REGIONS];
    size_t region_count;
    size_t total_managed_pages;
    size_t allocated_pages;
    size_t free_pages;
    size_t metadata_pages;
    phys_addr_t metadata_phys_start;
    phys_addr_t metadata_phys_end;
    bool initialized;
};

struct pmm_stats {
    size_t total_managed_pages; /* Pages across all managed usable regions */
    size_t allocated_pages;     /* Currently allocated pages (including PMM metadata) */
    size_t free_pages;          /* Pages currently available for allocation */
    size_t metadata_pages;      /* Pages reserved for PMM metadata and bitmaps */
    size_t region_count;        /* Number of managed physical memory regions */
};

/* Live Allocator Public API */
int pmm_init(struct limine_memmap_response *mmap, u64 hhdm_offset);
int pmm_alloc_page(phys_addr_t *out_paddr);
int pmm_free_page(phys_addr_t paddr);
int pmm_alloc_contiguous(size_t count, phys_addr_t *out_paddr);
int pmm_free_contiguous(phys_addr_t paddr, size_t count);
void pmm_get_stats(struct pmm_stats *out_stats);
void pmm_print_stats(void);
bool pmm_self_test(u64 hhdm_offset);
bool pmm_test_synthetic(u64 hhdm_offset);

/* Isolated Allocator Context API (for test fixtures / non-live allocators) */
int pmm_context_init(struct pmm_context *ctx, struct limine_memmap_response *mmap,
                     void *metadata_buffer, size_t metadata_buf_size,
                     phys_addr_t metadata_phys_base);
int pmm_context_alloc_page(struct pmm_context *ctx, phys_addr_t *out_paddr);
int pmm_context_free_page(struct pmm_context *ctx, phys_addr_t paddr);
int pmm_context_alloc_pages(struct pmm_context *ctx, size_t count, phys_addr_t *out_paddr);
int pmm_context_free_pages(struct pmm_context *ctx, phys_addr_t paddr, size_t count);
void pmm_context_get_stats(const struct pmm_context *ctx, struct pmm_stats *out_stats);

#endif /* ASTRA_KERNEL_PMM_H */
