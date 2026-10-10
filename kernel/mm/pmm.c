#include <kernel/pmm.h>
#include <kernel/kprintf.h>

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

static struct pmm_region *g_regions = NULL;
static size_t g_region_count = 0;
static size_t g_total_managed_pages = 0;
static size_t g_allocated_pages = 0;
static size_t g_free_pages = 0;
static size_t g_metadata_pages = 0;
static bool g_pmm_initialized = false;

static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

int pmm_init(struct limine_memmap_response *mmap, u64 hhdm_offset) {
    if (!mmap || mmap->entry_count == 0) {
        return PMM_ERR_INVALID_ARG;
    }

    (void)hhdm_offset;
    g_region_count = 0;
    g_total_managed_pages = 0;
    g_allocated_pages = 0;
    g_free_pages = 0;
    g_metadata_pages = 0;
    g_pmm_initialized = false;

    /* Temporary storage to discover usable regions */
    struct {
        phys_addr_t start;
        phys_addr_t end;
        size_t pages;
        size_t bmp_bytes;
    } candidates[MAX_PMM_REGIONS];

    size_t candidate_count = 0;
    size_t total_bitmap_bytes = 0;

    for (u64 i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (!e) continue;

        /* Filter: Only explicitly usable memory */
        if (e->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        /* Check integer overflow in range calculation */
        if (e->base + e->length < e->base) {
            continue;
        }

        /* Align start upward, end downward */
        phys_addr_t start = (e->base + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
        phys_addr_t end = (e->base + e->length) & ~(PAGE_SIZE - 1);

        /* Reserve physical page zero */
        if (start == 0) {
            start = PAGE_SIZE;
        }

        /* Discard empty or sub-page entries */
        if (end <= start) {
            continue;
        }

        size_t pages = (size_t)((end - start) / PAGE_SIZE);
        size_t bmp_bytes = (pages + 7) / 8;

        if (candidate_count < MAX_PMM_REGIONS) {
            candidates[candidate_count].start = start;
            candidates[candidate_count].end = end;
            candidates[candidate_count].pages = pages;
            candidates[candidate_count].bmp_bytes = bmp_bytes;
            total_bitmap_bytes += bmp_bytes;
            candidate_count++;
        }
    }

    if (candidate_count == 0) {
        return PMM_ERR_NOMEM;
    }

    /* Calculate metadata requirements */
    size_t regions_table_bytes = candidate_count * sizeof(struct pmm_region);
    size_t total_metadata_bytes = regions_table_bytes + total_bitmap_bytes;
    size_t metadata_pages = (total_metadata_bytes + (PAGE_SIZE - 1)) / PAGE_SIZE;

    /* Find a usable region with enough contiguous pages to host the metadata */
    int host_region_idx = -1;
    for (size_t i = 0; i < candidate_count; i++) {
        if (candidates[i].pages >= metadata_pages + 1) {
            host_region_idx = (int)i;
            break;
        }
    }

    if (host_region_idx < 0) {
        return PMM_ERR_NOMEM;
    }

    /* Place metadata at the start of the host region */
    phys_addr_t metadata_phys = candidates[host_region_idx].start;
    void *metadata_virt = (void *)(metadata_phys + hhdm_offset);

    /* Zero out all metadata memory */
    kmemset(metadata_virt, 0, metadata_pages * PAGE_SIZE);

    /* Set up regions table and bitmap pool in virtual memory */
    g_regions = (struct pmm_region *)metadata_virt;
    u8 *bitmap_pool = (u8 *)((uintptr_t)metadata_virt + regions_table_bytes);

    g_region_count = candidate_count;
    g_metadata_pages = metadata_pages;

    for (size_t i = 0; i < candidate_count; i++) {
        g_regions[i].base = candidates[i].start;
        g_regions[i].end = candidates[i].end;
        g_regions[i].total_pages = candidates[i].pages;
        g_regions[i].free_pages = candidates[i].pages;
        g_regions[i].last_search_idx = 0;
        g_regions[i].bitmap_bytes = candidates[i].bmp_bytes;
        g_regions[i].bitmap = bitmap_pool;
        bitmap_pool += candidates[i].bmp_bytes;

        g_total_managed_pages += candidates[i].pages;
    }

    /* Reserve metadata pages in host region bitmap immediately */
    for (size_t p = 0; p < metadata_pages; p++) {
        g_regions[host_region_idx].bitmap[p / 8] |= (1 << (p % 8));
    }
    g_regions[host_region_idx].free_pages -= metadata_pages;
    g_regions[host_region_idx].last_search_idx = metadata_pages;

    g_allocated_pages = metadata_pages;
    g_free_pages = g_total_managed_pages - metadata_pages;
    g_pmm_initialized = true;

    return PMM_SUCCESS;
}

int pmm_alloc_page(phys_addr_t *out_paddr) {
    if (!g_pmm_initialized || !out_paddr) {
        return PMM_ERR_INVALID_ARG;
    }

    if (g_free_pages == 0) {
        return PMM_ERR_NOMEM;
    }

    /* Single-core first-fit scan across managed regions */
    for (size_t r = 0; r < g_region_count; r++) {
        struct pmm_region *region = &g_regions[r];
        if (region->free_pages == 0) {
            continue;
        }

        size_t total = region->total_pages;
        size_t start_idx = region->last_search_idx;

        for (size_t offset = 0; offset < total; offset++) {
            size_t idx = (start_idx + offset) % total;
            size_t byte_idx = idx / 8;
            u8 bit_mask = (u8)(1 << (idx % 8));

            if ((region->bitmap[byte_idx] & bit_mask) == 0) {
                /* Allocate page */
                region->bitmap[byte_idx] |= bit_mask;
                region->free_pages--;
                region->last_search_idx = (idx + 1) % total;

                g_allocated_pages++;
                g_free_pages--;

                *out_paddr = region->base + ((phys_addr_t)idx * PAGE_SIZE);
                return PMM_SUCCESS;
            }
        }
    }

    return PMM_ERR_NOMEM;
}

int pmm_free_page(phys_addr_t paddr) {
    if (!g_pmm_initialized) {
        return PMM_ERR_INVALID_ARG;
    }

    if ((paddr & (PAGE_SIZE - 1)) != 0) {
        return PMM_ERR_UNALIGNED;
    }

    if (paddr == 0) {
        return PMM_ERR_PAGE_ZERO;
    }

    /* Locate region owning this physical address */
    for (size_t r = 0; r < g_region_count; r++) {
        struct pmm_region *region = &g_regions[r];
        if (paddr >= region->base && paddr < region->end) {
            size_t idx = (size_t)((paddr - region->base) / PAGE_SIZE);
            size_t byte_idx = idx / 8;
            u8 bit_mask = (u8)(1 << (idx % 8));

            if ((region->bitmap[byte_idx] & bit_mask) == 0) {
                return PMM_ERR_DOUBLE_FREE;
            }

            /* Clear bit */
            region->bitmap[byte_idx] &= ~bit_mask;
            region->free_pages++;
            if (idx < region->last_search_idx) {
                region->last_search_idx = idx;
            }

            g_allocated_pages--;
            g_free_pages++;

            return PMM_SUCCESS;
        }
    }

    return PMM_ERR_UNMANAGED;
}

void pmm_get_stats(struct pmm_stats *out_stats) {
    if (!out_stats) return;
    out_stats->total_managed_pages = g_total_managed_pages;
    out_stats->allocated_pages = g_allocated_pages;
    out_stats->free_pages = g_free_pages;
    out_stats->metadata_pages = g_metadata_pages;
    out_stats->region_count = g_region_count;
}

void pmm_print_stats(void) {
    if (!g_pmm_initialized) {
        kprintf("[PMM] Physical Memory Allocator not initialized.\n");
        return;
    }

    kprintf("\n=================== Physical Page Allocator Stats ===================\n");
    kprintf("Managed Regions:   %llu\n", (unsigned long long)g_region_count);
    kprintf("Total Managed:     %llu pages (%llu MiB)\n",
            (unsigned long long)g_total_managed_pages,
            (unsigned long long)((g_total_managed_pages * PAGE_SIZE) / (1024 * 1024)));
    kprintf("Allocated Pages:   %llu pages (%llu KiB)\n",
            (unsigned long long)g_allocated_pages,
            (unsigned long long)((g_allocated_pages * PAGE_SIZE) / 1024));
    kprintf("Free Pages:        %llu pages (%llu MiB)\n",
            (unsigned long long)g_free_pages,
            (unsigned long long)((g_free_pages * PAGE_SIZE) / (1024 * 1024)));
    kprintf("Metadata Overhead: %llu pages (%llu KiB)\n",
            (unsigned long long)g_metadata_pages,
            (unsigned long long)((g_metadata_pages * PAGE_SIZE) / 1024));
    kprintf("---------------------------------------------------------------------\n");

    for (size_t r = 0; r < g_region_count; r++) {
        kprintf("  Region [%02llu]: 0x%016llx .. 0x%016llx | %8llu / %8llu free\n",
                (unsigned long long)r,
                (unsigned long long)g_regions[r].base,
                (unsigned long long)g_regions[r].end,
                (unsigned long long)g_regions[r].free_pages,
                (unsigned long long)g_regions[r].total_pages);
    }
    kprintf("=====================================================================\n\n");
}

bool pmm_self_test(u64 hhdm_offset) {
    kprintf("[PMM] Starting Physical Memory Allocator Self-Test...\n");

    if (!g_pmm_initialized) {
        kprintf("[PMM] FAIL: PMM is not initialized.\n");
        return false;
    }

    struct pmm_stats initial_stats;
    pmm_get_stats(&initial_stats);

    #define TEST_ALLOC_COUNT 8
    phys_addr_t allocated[TEST_ALLOC_COUNT] = {0};

    /* 1. Allocate test pages */
    for (size_t i = 0; i < TEST_ALLOC_COUNT; i++) {
        int res = pmm_alloc_page(&allocated[i]);
        if (res != PMM_SUCCESS) {
            kprintf("[PMM] FAIL: pmm_alloc_page failed at index %llu (error %d)\n",
                    (unsigned long long)i, res);
            return false;
        }

        /* Assert alignment */
        if ((allocated[i] & (PAGE_SIZE - 1)) != 0) {
            kprintf("[PMM] FAIL: Allocated address 0x%016llx is not page-aligned\n",
                    (unsigned long long)allocated[i]);
            return false;
        }

        /* Assert page zero not handed out */
        if (allocated[i] == 0) {
            kprintf("[PMM] FAIL: Allocated address is page zero\n");
            return false;
        }

        /* Assert uniqueness */
        for (size_t j = 0; j < i; j++) {
            if (allocated[i] == allocated[j]) {
                kprintf("[PMM] FAIL: Duplicate page allocated: 0x%016llx\n",
                        (unsigned long long)allocated[i]);
                return false;
            }
        }

        /* 2. Write pattern to page through HHDM */
        u64 *virt_page = (u64 *)(allocated[i] + hhdm_offset);
        u64 test_pattern = 0xAA550000BEEF0000ULL | (u64)i;
        virt_page[0] = test_pattern;
        virt_page[511] = ~test_pattern;

        /* Verify pattern readback */
        if (virt_page[0] != test_pattern || virt_page[511] != ~test_pattern) {
            kprintf("[PMM] FAIL: Memory readback mismatch at 0x%016llx\n",
                    (unsigned long long)allocated[i]);
            return false;
        }
    }

    /* 3. Free allocated pages and verify error detection */
    for (size_t i = 0; i < TEST_ALLOC_COUNT; i++) {
        int res = pmm_free_page(allocated[i]);
        if (res != PMM_SUCCESS) {
            kprintf("[PMM] FAIL: pmm_free_page failed for 0x%016llx (error %d)\n",
                    (unsigned long long)allocated[i], res);
            return false;
        }

        /* Test double free */
        int df_res = pmm_free_page(allocated[i]);
        if (df_res != PMM_ERR_DOUBLE_FREE) {
            kprintf("[PMM] FAIL: Double free did not return PMM_ERR_DOUBLE_FREE (got %d)\n", df_res);
            return false;
        }
    }

    /* 4. Test error paths */
    if (pmm_free_page(allocated[0] + 1) != PMM_ERR_UNALIGNED) {
        kprintf("[PMM] FAIL: Unaligned free did not return PMM_ERR_UNALIGNED\n");
        return false;
    }

    if (pmm_free_page(0) != PMM_ERR_PAGE_ZERO) {
        kprintf("[PMM] FAIL: Page zero free did not return PMM_ERR_PAGE_ZERO\n");
        return false;
    }

    if (pmm_free_page(0x000000fd00000000ULL) != PMM_ERR_UNMANAGED) {
        kprintf("[PMM] FAIL: Reserved MMIO free did not return PMM_ERR_UNMANAGED\n");
        return false;
    }

    /* 5. Assert accounting returned exactly to baseline */
    struct pmm_stats final_stats;
    pmm_get_stats(&final_stats);

    if (final_stats.free_pages != initial_stats.free_pages ||
        final_stats.allocated_pages != initial_stats.allocated_pages) {
        kprintf("[PMM] FAIL: Accounting mismatch after self-test. Initial free=%llu, final free=%llu\n",
                (unsigned long long)initial_stats.free_pages,
                (unsigned long long)final_stats.free_pages);
        return false;
    }

    kprintf("[PMM] Self-test passed: %llu pages cycled, error traps verified, accounting intact.\n",
            (unsigned long long)TEST_ALLOC_COUNT);
    return true;
}
