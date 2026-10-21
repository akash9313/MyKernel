#include <kernel/pmm.h>
#include <kernel/kprintf.h>
#include <kernel/arch/x86_64/cpu.h>

static struct pmm_context g_live_pmm;

static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

int pmm_context_init(struct pmm_context *ctx, struct limine_memmap_response *mmap,
                     void *metadata_buffer, size_t metadata_buf_size,
                     phys_addr_t metadata_phys_base) {
    if (!ctx || !mmap || mmap->entry_count == 0 || !metadata_buffer) {
        return PMM_ERR_INVALID_ARG;
    }

    kmemset(ctx, 0, sizeof(*ctx));

    /* Temporary storage for discovered candidate regions */
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

    /* Verify metadata buffer has enough space for all region bitmaps */
    if (metadata_buf_size < total_bitmap_bytes) {
        return PMM_ERR_NOMEM;
    }

    kmemset(metadata_buffer, 0, metadata_buf_size);

    u8 *bitmap_pool = (u8 *)metadata_buffer;
    ctx->region_count = candidate_count;
    ctx->total_managed_pages = 0;

    for (size_t i = 0; i < candidate_count; i++) {
        ctx->regions[i].base = candidates[i].start;
        ctx->regions[i].end = candidates[i].end;
        ctx->regions[i].total_pages = candidates[i].pages;
        ctx->regions[i].free_pages = candidates[i].pages;
        ctx->regions[i].last_search_idx = 0;
        ctx->regions[i].bitmap_bytes = candidates[i].bmp_bytes;
        ctx->regions[i].bitmap = bitmap_pool;
        bitmap_pool += candidates[i].bmp_bytes;

        ctx->total_managed_pages += candidates[i].pages;
    }

    /* Calculate metadata pages reserved */
    size_t metadata_pages = (metadata_buf_size + (PAGE_SIZE - 1)) / PAGE_SIZE;
    ctx->metadata_pages = metadata_pages;
    ctx->metadata_phys_start = metadata_phys_base;
    ctx->metadata_phys_end = metadata_phys_base + (metadata_pages * PAGE_SIZE);

    /* If metadata resides inside one of the managed regions, mark it reserved */
    for (size_t r = 0; r < candidate_count; r++) {
        if (metadata_phys_base >= ctx->regions[r].base && metadata_phys_base < ctx->regions[r].end) {
            size_t start_p = (size_t)((metadata_phys_base - ctx->regions[r].base) / PAGE_SIZE);
            for (size_t p = 0; p < metadata_pages && (start_p + p) < ctx->regions[r].total_pages; p++) {
                size_t p_idx = start_p + p;
                ctx->regions[r].bitmap[p_idx / 8] |= (1 << (p_idx % 8));
                ctx->regions[r].free_pages--;
            }
            ctx->regions[r].last_search_idx = start_p + metadata_pages;
            ctx->allocated_pages += metadata_pages;
            break;
        }
    }

    ctx->free_pages = ctx->total_managed_pages - ctx->allocated_pages;
    ctx->initialized = true;

    return PMM_SUCCESS;
}

int pmm_context_alloc_page(struct pmm_context *ctx, phys_addr_t *out_paddr) {
    if (!ctx || !ctx->initialized || !out_paddr) {
        return PMM_ERR_INVALID_ARG;
    }

    if (ctx->free_pages == 0) {
        return PMM_ERR_NOMEM;
    }

    for (size_t r = 0; r < ctx->region_count; r++) {
        struct pmm_region *region = &ctx->regions[r];
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
                region->bitmap[byte_idx] |= bit_mask;
                region->free_pages--;
                region->last_search_idx = (idx + 1) % total;

                ctx->allocated_pages++;
                ctx->free_pages--;

                *out_paddr = region->base + ((phys_addr_t)idx * PAGE_SIZE);
                return PMM_SUCCESS;
            }
        }
    }

    return PMM_ERR_NOMEM;
}

int pmm_context_alloc_pages(struct pmm_context *ctx, size_t count, phys_addr_t *out_paddr) {
    if (!ctx || !ctx->initialized || !out_paddr || count == 0) {
        return PMM_ERR_INVALID_ARG;
    }

    if (count == 1) {
        return pmm_context_alloc_page(ctx, out_paddr);
    }

    if (ctx->free_pages < count) {
        return PMM_ERR_NOMEM;
    }

    for (size_t r = 0; r < ctx->region_count; r++) {
        struct pmm_region *region = &ctx->regions[r];
        if (region->free_pages < count) {
            continue;
        }

        size_t total = region->total_pages;
        size_t run_start = 0;
        size_t run_len = 0;

        for (size_t idx = 0; idx < total; idx++) {
            size_t byte_idx = idx / 8;
            u8 bit_mask = (u8)(1 << (idx % 8));

            if ((region->bitmap[byte_idx] & bit_mask) == 0) {
                if (run_len == 0) {
                    run_start = idx;
                }
                run_len++;
                if (run_len == count) {
                    for (size_t p = 0; p < count; p++) {
                        size_t p_idx = run_start + p;
                        region->bitmap[p_idx / 8] |= (1 << (p_idx % 8));
                    }
                    region->free_pages -= count;
                    ctx->allocated_pages += count;
                    ctx->free_pages -= count;
                    *out_paddr = region->base + ((phys_addr_t)run_start * PAGE_SIZE);
                    return PMM_SUCCESS;
                }
            } else {
                run_len = 0;
            }
        }
    }

    return PMM_ERR_NOMEM;
}

int pmm_context_free_pages(struct pmm_context *ctx, phys_addr_t paddr, size_t count) {
    if (!ctx || !ctx->initialized || count == 0) {
        return PMM_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        int res = pmm_context_free_page(ctx, paddr + (i * PAGE_SIZE));
        if (res != PMM_SUCCESS) return res;
    }
    return PMM_SUCCESS;
}

int pmm_context_free_page(struct pmm_context *ctx, phys_addr_t paddr) {
    if (!ctx || !ctx->initialized) {
        return PMM_ERR_INVALID_ARG;
    }

    if ((paddr & (PAGE_SIZE - 1)) != 0) {
        return PMM_ERR_UNALIGNED;
    }

    if (paddr == 0) {
        return PMM_ERR_PAGE_ZERO;
    }

    /* Reject freeing allocator metadata or permanent reservations */
    if (ctx->metadata_phys_start != 0 &&
        paddr >= ctx->metadata_phys_start &&
        paddr < ctx->metadata_phys_end) {
        return PMM_ERR_RESERVED;
    }

    for (size_t r = 0; r < ctx->region_count; r++) {
        struct pmm_region *region = &ctx->regions[r];
        if (paddr >= region->base && paddr < region->end) {
            size_t idx = (size_t)((paddr - region->base) / PAGE_SIZE);
            size_t byte_idx = idx / 8;
            u8 bit_mask = (u8)(1 << (idx % 8));

            if ((region->bitmap[byte_idx] & bit_mask) == 0) {
                return PMM_ERR_DOUBLE_FREE;
            }

            region->bitmap[byte_idx] &= ~bit_mask;
            region->free_pages++;
            if (idx < region->last_search_idx) {
                region->last_search_idx = idx;
            }

            ctx->allocated_pages--;
            ctx->free_pages++;

            return PMM_SUCCESS;
        }
    }

    return PMM_ERR_UNMANAGED;
}

void pmm_context_get_stats(const struct pmm_context *ctx, struct pmm_stats *out_stats) {
    if (!ctx || !out_stats) return;
    out_stats->total_managed_pages = ctx->total_managed_pages;
    out_stats->allocated_pages = ctx->allocated_pages;
    out_stats->free_pages = ctx->free_pages;
    out_stats->metadata_pages = ctx->metadata_pages;
    out_stats->region_count = ctx->region_count;
}

/* --- Live Allocator Implementation --- */

int pmm_init(struct limine_memmap_response *mmap, u64 hhdm_offset) {
    if (!mmap || mmap->entry_count == 0) {
        return PMM_ERR_INVALID_ARG;
    }

    /* Calculate total bitmap bytes needed across all usable regions */
    size_t total_bmp_bytes = 0;
    size_t valid_entries = 0;

    for (u64 i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE) continue;
        if (e->base + e->length < e->base) continue;

        phys_addr_t start = (e->base + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
        phys_addr_t end = (e->base + e->length) & ~(PAGE_SIZE - 1);
        if (start == 0) start = PAGE_SIZE;
        if (end <= start) continue;

        size_t pages = (size_t)((end - start) / PAGE_SIZE);
        total_bmp_bytes += (pages + 7) / 8;
        valid_entries++;
    }

    if (valid_entries == 0) {
        return PMM_ERR_NOMEM;
    }

    size_t metadata_pages = (total_bmp_bytes + (PAGE_SIZE - 1)) / PAGE_SIZE;
    size_t metadata_buf_size = metadata_pages * PAGE_SIZE;

    /* Find a usable region with enough contiguous pages to host the metadata */
    phys_addr_t host_phys = 0;
    bool found_host = false;

    for (u64 i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE) continue;
        if (e->base + e->length < e->base) continue;

        phys_addr_t start = (e->base + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
        phys_addr_t end = (e->base + e->length) & ~(PAGE_SIZE - 1);
        if (start == 0) start = PAGE_SIZE;
        if (end <= start) continue;

        size_t pages = (size_t)((end - start) / PAGE_SIZE);
        if (pages >= metadata_pages + 1) {
            host_phys = start;
            found_host = true;
            break;
        }
    }

    if (!found_host) {
        return PMM_ERR_NOMEM;
    }

    void *metadata_virt = (void *)(host_phys + hhdm_offset);

    return pmm_context_init(&g_live_pmm, mmap, metadata_virt, metadata_buf_size, host_phys);
}

int pmm_alloc_page(phys_addr_t *out_paddr) {
    u64 flags = irq_save();
    int ret = pmm_context_alloc_page(&g_live_pmm, out_paddr);
    irq_restore(flags);
    return ret;
}

int pmm_free_page(phys_addr_t paddr) {
    u64 flags = irq_save();
    int ret = pmm_context_free_page(&g_live_pmm, paddr);
    irq_restore(flags);
    return ret;
}

int pmm_alloc_contiguous(size_t count, phys_addr_t *out_paddr) {
    u64 flags = irq_save();
    int ret = pmm_context_alloc_pages(&g_live_pmm, count, out_paddr);
    irq_restore(flags);
    return ret;
}

int pmm_free_contiguous(phys_addr_t paddr, size_t count) {
    u64 flags = irq_save();
    int ret = pmm_context_free_pages(&g_live_pmm, paddr, count);
    irq_restore(flags);
    return ret;
}

void pmm_get_stats(struct pmm_stats *out_stats) {
    pmm_context_get_stats(&g_live_pmm, out_stats);
}

void pmm_print_stats(void) {
    if (!g_live_pmm.initialized) {
        kprintf("[PMM] Physical Memory Allocator not initialized.\n");
        return;
    }

    kprintf("\n=================== Physical Page Allocator Stats ===================\n");
    kprintf("Managed Regions:   %llu\n", (unsigned long long)g_live_pmm.region_count);
    kprintf("Total Managed:     %llu pages (%llu MiB)\n",
            (unsigned long long)g_live_pmm.total_managed_pages,
            (unsigned long long)((g_live_pmm.total_managed_pages * PAGE_SIZE) / (1024 * 1024)));
    kprintf("Allocated Pages:   %llu pages (%llu KiB)\n",
            (unsigned long long)g_live_pmm.allocated_pages,
            (unsigned long long)((g_live_pmm.allocated_pages * PAGE_SIZE) / 1024));
    kprintf("Free Pages:        %llu pages (%llu MiB)\n",
            (unsigned long long)g_live_pmm.free_pages,
            (unsigned long long)((g_live_pmm.free_pages * PAGE_SIZE) / (1024 * 1024)));
    kprintf("Metadata Overhead: %llu pages (%llu KiB)\n",
            (unsigned long long)g_live_pmm.metadata_pages,
            (unsigned long long)((g_live_pmm.metadata_pages * PAGE_SIZE) / 1024));
    kprintf("Metadata Range:    0x%016llx .. 0x%016llx\n",
            (unsigned long long)g_live_pmm.metadata_phys_start,
            (unsigned long long)g_live_pmm.metadata_phys_end);
    kprintf("---------------------------------------------------------------------\n");

    for (size_t r = 0; r < g_live_pmm.region_count; r++) {
        kprintf("  Region [%02llu]: 0x%016llx .. 0x%016llx | %8llu / %8llu free\n",
                (unsigned long long)r,
                (unsigned long long)g_live_pmm.regions[r].base,
                (unsigned long long)g_live_pmm.regions[r].end,
                (unsigned long long)g_live_pmm.regions[r].free_pages,
                (unsigned long long)g_live_pmm.regions[r].total_pages);
    }
    kprintf("=====================================================================\n\n");
}

/* --- Isolated Synthetic Allocator Verification (Runs in QEMU without touching live RAM) --- */

bool pmm_test_synthetic(u64 hhdm_offset) {
    kprintf("[PMM] Running Isolated Synthetic C Allocator Tests in QEMU...\n");

    /* Allocate 1 scratch page from the live PMM to hold test bitmaps */
    phys_addr_t scratch_phys;
    int a_res = pmm_alloc_page(&scratch_phys);
    if (a_res != PMM_SUCCESS) {
        kprintf("[PMM] FAIL: Cannot allocate scratch page for synthetic test\n");
        return false;
    }
    void *scratch_virt = (void *)(scratch_phys + hhdm_offset);

    /* 1. Test unaligned, tiny, and arithmetic overflow descriptors */
    struct limine_memmap_entry entry_unaligned = {
        .base = 0x1000100ULL,
        .length = 5000ULL, /* Starts at 0x1000100 -> aligns up to 0x1001000; ends at 0x1001488 -> aligns down to 0x1001000 -> 0 pages */
        .type = LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry entry_valid_unaligned = {
        .base = 0x2000800ULL,
        .length = 0x3000ULL, /* 0x2000800 -> 0x2001000 .. 0x2003800 -> 0x2003000 (2 pages) */
        .type = LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry entry_tiny = {
        .base = 0x3000000ULL,
        .length = 512ULL, /* < 4096 bytes: discarded */
        .type = LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry entry_overflow = {
        .base = 0xFFFFFFFFFFFFF000ULL,
        .length = 0x2000ULL, /* Overflows 64-bit integer: discarded */
        .type = LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry entry_reserved = {
        .base = 0x4000000ULL,
        .length = 0x10000ULL,
        .type = LIMINE_MEMMAP_RESERVED /* Non-usable type: ignored */
    };

    struct limine_memmap_entry *syn_entries1[] = {
        &entry_unaligned,
        &entry_valid_unaligned,
        &entry_tiny,
        &entry_overflow,
        &entry_reserved
    };
    struct limine_memmap_response syn_mmap1 = {
        .entry_count = 5,
        .entries = syn_entries1
    };

    struct pmm_context syn_ctx;
    int syn_res = pmm_context_init(&syn_ctx, &syn_mmap1, scratch_virt, PAGE_SIZE, 0);
    if (syn_res != PMM_SUCCESS) {
        kprintf("[PMM] FAIL: Synthetic context init failed (err %d)\n", syn_res);
        pmm_free_page(scratch_phys);
        return false;
    }

    /* Only entry_valid_unaligned should have yielded pages: exactly 2 pages */
    if (syn_ctx.region_count != 1 || syn_ctx.total_managed_pages != 2) {
        kprintf("[PMM] FAIL: Synthetic region parsing error (regions=%llu, pages=%llu)\n",
                (unsigned long long)syn_ctx.region_count,
                (unsigned long long)syn_ctx.total_managed_pages);
        pmm_free_page(scratch_phys);
        return false;
    }

    /* 2. Test Exhaustion and Recovery on synthetic context */
    phys_addr_t p1 = 0, p2 = 0, p3 = 0;
    if (pmm_context_alloc_page(&syn_ctx, &p1) != PMM_SUCCESS ||
        pmm_context_alloc_page(&syn_ctx, &p2) != PMM_SUCCESS) {
        kprintf("[PMM] FAIL: Failed allocating synthetic pages\n");
        pmm_free_page(scratch_phys);
        return false;
    }
    if (pmm_context_alloc_page(&syn_ctx, &p3) != PMM_ERR_NOMEM) {
        kprintf("[PMM] FAIL: Exhaustion did not return PMM_ERR_NOMEM\n");
        pmm_free_page(scratch_phys);
        return false;
    }

    /* Free p1 and recover */
    if (pmm_context_free_page(&syn_ctx, p1) != PMM_SUCCESS) {
        kprintf("[PMM] FAIL: Freeing synthetic page failed\n");
        pmm_free_page(scratch_phys);
        return false;
    }
    if (pmm_context_alloc_page(&syn_ctx, &p3) != PMM_SUCCESS || p3 != p1) {
        kprintf("[PMM] FAIL: Recovery allocation failed or did not reuse freed slot\n");
        pmm_free_page(scratch_phys);
        return false;
    }
    pmm_context_free_page(&syn_ctx, p2);
    pmm_context_free_page(&syn_ctx, p3);

    /* 3. Test Sparse Address-Space Handling without dereferencing nonexistent RAM */
    /* Base at 1 TiB (0x10000000000ULL). Only bitmap in scratch_virt is touched. */
    struct limine_memmap_entry entry_sparse = {
        .base = 0x10000000000ULL,
        .length = 4 * PAGE_SIZE,
        .type = LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry *syn_entries2[] = { &entry_sparse };
    struct limine_memmap_response syn_mmap2 = { .entry_count = 1, .entries = syn_entries2 };

    struct pmm_context sparse_ctx;
    if (pmm_context_init(&sparse_ctx, &syn_mmap2, scratch_virt, PAGE_SIZE, 0) != PMM_SUCCESS) {
        kprintf("[PMM] FAIL: Sparse context init failed\n");
        pmm_free_page(scratch_phys);
        return false;
    }

    phys_addr_t sparse_paddr;
    if (pmm_context_alloc_page(&sparse_ctx, &sparse_paddr) != PMM_SUCCESS ||
        sparse_paddr != 0x10000000000ULL) {
        kprintf("[PMM] FAIL: Sparse allocation address mismatch (0x%016llx)\n", (unsigned long long)sparse_paddr);
        pmm_free_page(scratch_phys);
        return false;
    }
    if (pmm_context_free_page(&sparse_ctx, sparse_paddr) != PMM_SUCCESS) {
        kprintf("[PMM] FAIL: Sparse free failed\n");
        pmm_free_page(scratch_phys);
        return false;
    }

    /* 4. Test Permanent Metadata Reservation Protection */
    struct limine_memmap_entry entry_res_test = {
        .base = 0x5000000ULL,
        .length = 8 * PAGE_SIZE,
        .type = LIMINE_MEMMAP_USABLE
    };
    struct limine_memmap_entry *syn_entries3[] = { &entry_res_test };
    struct limine_memmap_response syn_mmap3 = { .entry_count = 1, .entries = syn_entries3 };

    struct pmm_context res_ctx;
    /* Host metadata in the middle of region at 0x5002000 (1 page) */
    if (pmm_context_init(&res_ctx, &syn_mmap3, scratch_virt, PAGE_SIZE, 0x5002000ULL) != PMM_SUCCESS) {
        kprintf("[PMM] FAIL: Reservation context init failed\n");
        pmm_free_page(scratch_phys);
        return false;
    }

    /* Attempting to free reserved metadata page MUST return PMM_ERR_RESERVED */
    int res_free = pmm_context_free_page(&res_ctx, 0x5002000ULL);
    if (res_free != PMM_ERR_RESERVED) {
        kprintf("[PMM] FAIL: Freeing reserved metadata did not return PMM_ERR_RESERVED (got %d)\n", res_free);
        pmm_free_page(scratch_phys);
        return false;
    }

    /* Clean up scratch page to live allocator */
    pmm_free_page(scratch_phys);

    kprintf("[PMM] Isolated synthetic C allocator tests passed: all edge cases verified safely.\n");
    return true;
}

bool pmm_self_test(u64 hhdm_offset) {
    kprintf("[PMM] Starting Physical Memory Allocator Self-Test...\n");

    if (!g_live_pmm.initialized) {
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

    /* Test metadata rejection on live allocator */
    if (g_live_pmm.metadata_phys_start != 0) {
        int meta_res = pmm_free_page(g_live_pmm.metadata_phys_start);
        if (meta_res != PMM_ERR_RESERVED) {
            kprintf("[PMM] FAIL: Freeing live metadata page did not return PMM_ERR_RESERVED (got %d)\n", meta_res);
            return false;
        }
    }

    /* 5. Run isolated synthetic C tests */
    if (!pmm_test_synthetic(hhdm_offset)) {
        return false;
    }

    /* 6. Assert accounting returned exactly to baseline */
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
