#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include <kernel/types.h>
#include <kernel/pmm.h>
#include <kernel/kprintf.h>
#include <limine.h>

/* Mock kprintf implementation for host tests */
void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* Helper to build synthetic memory map */
#define MAX_ENTRIES 32
static struct limine_memmap_entry g_test_entries[MAX_ENTRIES];
static struct limine_memmap_entry *g_test_ptrs[MAX_ENTRIES];
static struct limine_memmap_response g_test_mmap;

static void reset_mmap(void) {
    memset(g_test_entries, 0, sizeof(g_test_entries));
    for (int i = 0; i < MAX_ENTRIES; i++) {
        g_test_ptrs[i] = &g_test_entries[i];
    }
    g_test_mmap.revision = 0;
    g_test_mmap.entry_count = 0;
    g_test_mmap.entries = g_test_ptrs;
}

static void add_entry(uint64_t base, uint64_t length, uint64_t type) {
    uint64_t idx = g_test_mmap.entry_count++;
    assert(idx < MAX_ENTRIES);
    g_test_entries[idx].base = base;
    g_test_entries[idx].length = length;
    g_test_entries[idx].type = type;
}

/* Simulated physical memory buffer for host testing (simulates HHDM) */
static u8 g_simulated_phys_mem[32 * 1024 * 1024]; /* 32 MB simulated RAM */

static void test_unaligned_and_tiny_regions(void) {
    printf("[HOST TEST] Running test_unaligned_and_tiny_regions...\n");
    reset_mmap();

    /* Unaligned start (0x100500) -> should align up to 0x101000 */
    /* Length 0x4800 -> ends at 0x104D00 -> should align down to 0x104000 (3 pages: 0x101000, 0x102000, 0x103000) */
    add_entry(0x100500, 0x4800, LIMINE_MEMMAP_USABLE);

    /* Tiny region < 1 page (0x200000, 2048 bytes) -> should be discarded */
    add_entry(0x200000, 2048, LIMINE_MEMMAP_USABLE);

    /* Another unaligned region (0x300800, 100 bytes) -> end < start -> should be discarded */
    add_entry(0x300800, 100, LIMINE_MEMMAP_USABLE);

    /* Larger usable region to hold metadata and pages */
    /* 0x400000 .. 0x600000 (2 MB = 512 pages) */
    add_entry(0x400000, 0x200000, LIMINE_MEMMAP_USABLE);

    /* HHDM offset maps simulated RAM directly: addr + offset = pointer into g_simulated_phys_mem */
    u64 hhdm = (u64)(uintptr_t)g_simulated_phys_mem;

    int res = pmm_init(&g_test_mmap, hhdm);
    assert(res == PMM_SUCCESS);

    struct pmm_stats st;
    pmm_get_stats(&st);
    /* 3 pages from first region + 512 pages from second region = 515 pages total */
    assert(st.total_managed_pages == 515);
    assert(st.region_count == 2);
    assert(st.metadata_pages > 0);
    assert(st.free_pages == st.total_managed_pages - st.metadata_pages);
    printf("  PASS: Unaligned and tiny regions correctly aligned/filtered.\n");
}

static void test_sparse_high_address_regions(void) {
    printf("[HOST TEST] Running test_sparse_high_address_regions...\n");
    reset_mmap();

    /* Usable region 1: 0x100000 .. 0x200000 (1 MB) */
    add_entry(0x100000, 0x100000, LIMINE_MEMMAP_USABLE);

    /* Reserved gap: 0x200000 .. 0x800000 */
    add_entry(0x200000, 0x600000, LIMINE_MEMMAP_RESERVED);

    /* Usable region 2: 0x800000 .. 0xA00000 (2 MB) */
    add_entry(0x800000, 0x200000, LIMINE_MEMMAP_USABLE);

    /* Gigantic high MMIO aperture: 0xfd00000000 .. 0xffffffffff (12 GB) - MUST NOT bloat metadata! */
    add_entry(0xfd00000000ULL, 0x300000000ULL, LIMINE_MEMMAP_RESERVED);

    u64 hhdm = (u64)(uintptr_t)g_simulated_phys_mem;
    int res = pmm_init(&g_test_mmap, hhdm);
    assert(res == PMM_SUCCESS);

    struct pmm_stats st;
    pmm_get_stats(&st);
    /* Only regions 1 and 2 are managed: 256 + 512 = 768 pages = 3 MB */
    assert(st.total_managed_pages == 768);
    assert(st.region_count == 2);
    /* Metadata must only be 1 page (768 bits = 96 bytes bitmap + descriptors)! */
    assert(st.metadata_pages == 1);
    printf("  PASS: Sparse regions handled; high MMIO ignored without memory bloat.\n");
}

static void test_overflow_and_invalid_ranges(void) {
    printf("[HOST TEST] Running test_overflow_and_invalid_ranges...\n");
    reset_mmap();

    /* Overflow: base + length wraps around */
    add_entry(0xFFFFFFFFFFFFF000ULL, 0x2000, LIMINE_MEMMAP_USABLE);

    /* Zero length */
    add_entry(0x500000, 0, LIMINE_MEMMAP_USABLE);

    /* Valid host region */
    add_entry(0x100000, 0x100000, LIMINE_MEMMAP_USABLE);

    u64 hhdm = (u64)(uintptr_t)g_simulated_phys_mem;
    int res = pmm_init(&g_test_mmap, hhdm);
    assert(res == PMM_SUCCESS);

    struct pmm_stats st;
    pmm_get_stats(&st);
    assert(st.region_count == 1);
    assert(st.total_managed_pages == 256);
    printf("  PASS: Overflow and zero-length ranges safely rejected.\n");
}

static void test_exhaustion_and_recovery(void) {
    printf("[HOST TEST] Running test_exhaustion_and_recovery...\n");
    reset_mmap();

    /* A small usable region of 10 pages: 0x100000 .. 0x10A000 */
    add_entry(0x100000, 10 * PAGE_SIZE, LIMINE_MEMMAP_USABLE);

    u64 hhdm = (u64)(uintptr_t)g_simulated_phys_mem;
    int res = pmm_init(&g_test_mmap, hhdm);
    assert(res == PMM_SUCCESS);

    struct pmm_stats st;
    pmm_get_stats(&st);
    size_t free_before = st.free_pages;
    assert(free_before > 0);

    phys_addr_t allocated[32];
    for (size_t i = 0; i < free_before; i++) {
        int r = pmm_alloc_page(&allocated[i]);
        assert(r == PMM_SUCCESS);
        assert(allocated[i] >= 0x100000 && allocated[i] < 0x10A000);
    }

    /* Pool must now be completely exhausted */
    phys_addr_t overflow_page;
    int fail_res = pmm_alloc_page(&overflow_page);
    assert(fail_res == PMM_ERR_NOMEM);

    pmm_get_stats(&st);
    assert(st.free_pages == 0);

    /* Free one page and verify recovery */
    int free_res = pmm_free_page(allocated[0]);
    assert(free_res == PMM_SUCCESS);

    pmm_get_stats(&st);
    assert(st.free_pages == 1);

    /* Now allocation must succeed again */
    phys_addr_t recovered_page;
    int rec_res = pmm_alloc_page(&recovered_page);
    assert(rec_res == PMM_SUCCESS);
    assert(recovered_page == allocated[0]);

    /* Clean up remaining */
    pmm_free_page(recovered_page);
    for (size_t i = 1; i < free_before; i++) {
        assert(pmm_free_page(allocated[i]) == PMM_SUCCESS);
    }

    pmm_get_stats(&st);
    assert(st.free_pages == free_before);
    printf("  PASS: Exhaustion correctly detected; recovery functional.\n");
}

static void test_invalid_and_double_frees(void) {
    printf("[HOST TEST] Running test_invalid_and_double_frees...\n");
    reset_mmap();

    add_entry(0x100000, 16 * PAGE_SIZE, LIMINE_MEMMAP_USABLE);

    u64 hhdm = (u64)(uintptr_t)g_simulated_phys_mem;
    int res = pmm_init(&g_test_mmap, hhdm);
    assert(res == PMM_SUCCESS);

    phys_addr_t p;
    assert(pmm_alloc_page(&p) == PMM_SUCCESS);

    /* Unaligned free */
    assert(pmm_free_page(p + 1) == PMM_ERR_UNALIGNED);
    assert(pmm_free_page(p + 1024) == PMM_ERR_UNALIGNED);

    /* Page zero free */
    assert(pmm_free_page(0) == PMM_ERR_PAGE_ZERO);

    /* Free address outside any managed region */
    assert(pmm_free_page(0x200000) == PMM_ERR_UNMANAGED);
    assert(pmm_free_page(0xFED00000ULL) == PMM_ERR_UNMANAGED);

    /* Valid free */
    assert(pmm_free_page(p) == PMM_SUCCESS);

    /* Double free */
    assert(pmm_free_page(p) == PMM_ERR_DOUBLE_FREE);

    printf("  PASS: Unaligned, page zero, unmanaged, and double frees correctly rejected.\n");
}

int main(void) {
    printf("======================================================\n");
    printf("       AstraOS Physical Memory Manager Host Tests     \n");
    printf("======================================================\n");

    test_unaligned_and_tiny_regions();
    test_sparse_high_address_regions();
    test_overflow_and_invalid_ranges();
    test_exhaustion_and_recovery();
    test_invalid_and_double_frees();

    printf("------------------------------------------------------\n");
    printf("[SUCCESS] All host-side PMM unit tests passed!\n");
    printf("======================================================\n");
    return 0;
}
