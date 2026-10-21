#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>

#include <kernel/types.h>
#include <kernel/heap.h>

/* Mock / Stub functions for compiling kernel/mm/heap.c on host */
static jmp_buf g_panic_jmp;
static bool g_expect_panic = false;
static int g_panics_caught = 0;
static char g_last_panic_msg[256] = {0};

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void kernel_panic(const char *file, int line, const char *fmt, ...) {
    (void)file;
    (void)line;
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_panic_msg, sizeof(g_last_panic_msg), fmt, args);
    va_end(args);

    if (g_expect_panic) {
        g_panics_caught++;
        longjmp(g_panic_jmp, 1);
    } else {
        fprintf(stderr, "\n[FATAL PANIC at %s:%d] %s\n", file, line, g_last_panic_msg);
        abort();
    }
}

/* Stubs for arch/PMM/VMM/preempt symbols (unused by heap_core_* / kmalloc on host) */
u64 read_cr3(void) { return 0; }
void invlpg(virt_addr_t va) { (void)va; }
int pmm_alloc_page(phys_addr_t *out) { (void)out; return 0; }
int pmm_free_page(phys_addr_t p) { (void)p; return 0; }
int vmm_map_page(phys_addr_t r, virt_addr_t v, phys_addr_t p, u64 f) { (void)r; (void)v; (void)p; (void)f; return 0; }
int vmm_unmap_page(phys_addr_t r, virt_addr_t v) { (void)r; (void)v; return 0; }

static u32 g_host_preempt_count = 0;
void preempt_disable(void) { g_host_preempt_count++; }
void preempt_enable(void) { if (g_host_preempt_count > 0) g_host_preempt_count--; }
u32 preempt_count(void) { return g_host_preempt_count; }
bool in_interrupt(void) { return false; }

/* 64 KiB Host-aligned arena buffer */
static uint8_t g_host_arena[HEAP_ARENA_SIZE + 4096] __attribute__((aligned(4096)));
static void *g_aligned_arena = NULL;

static void setup_fresh_arena(void) {
    uintptr_t addr = (uintptr_t)g_host_arena;
    if ((addr % 4096) != 0) {
        addr = (addr + 4095) & ~4095ULL;
    }
    g_aligned_arena = (void *)addr;
    memset(g_aligned_arena, 0xA5, HEAP_ARENA_SIZE);
    bool ok = heap_core_init(g_aligned_arena, HEAP_ARENA_SIZE);
    assert(ok);
    assert(heap_validate());
}

static void test_zero_and_null_behavior(void) {
    printf("[HOST TEST] 1. Zero-size and NULL semantics...\n");
    setup_fresh_arena();

    assert(kmalloc(0) == NULL);
    assert(heap_validate());

    kfree(NULL); /* Safe no-op */
    assert(heap_validate());

    assert(kcalloc(0, 100) == NULL);
    assert(kcalloc(100, 0) == NULL);
    assert(kcalloc(0, 0) == NULL);
    assert(heap_validate());

    /* krealloc with NULL pointer behaves like kmalloc */
    void *p = krealloc(NULL, 64);
    assert(p != NULL);
    assert(((uintptr_t)p % HEAP_ALIGNMENT) == 0);
    assert(heap_validate());

    /* krealloc with size 0 frees pointer and returns NULL */
    void *p_freed = krealloc(p, 0);
    assert(p_freed == NULL);
    assert(heap_validate());

    struct heap_stats st;
    heap_get_stats(&st);
    assert(st.live_allocations == 0);
    assert(st.free_blocks == 1);
    printf("  PASS: Zero-size and NULL semantics verified.\n");
}

static void test_alignment_and_overflow(void) {
    printf("[HOST TEST] 2. Alignment and arithmetic overflow checks...\n");
    setup_fresh_arena();

    /* Test alignments of various sizes: 1, 7, 15, 16, 17, 31, 32, 63, 64, 128 */
    size_t test_sizes[] = { 1, 7, 15, 16, 17, 31, 32, 63, 64, 128, 256, 512, 1024 };
    void *ptrs[sizeof(test_sizes)/sizeof(test_sizes[0])];
    for (size_t i = 0; i < sizeof(test_sizes)/sizeof(test_sizes[0]); i++) {
        ptrs[i] = kmalloc(test_sizes[i]);
        assert(ptrs[i] != NULL);
        assert(((uintptr_t)ptrs[i] % HEAP_ALIGNMENT) == 0);
        memset(ptrs[i], 0x5A, test_sizes[i]);
    }
    assert(heap_validate());

    for (size_t i = 0; i < sizeof(test_sizes)/sizeof(test_sizes[0]); i++) {
        kfree(ptrs[i]);
    }
    assert(heap_validate());

    /* Overflow rejection */
    assert(kmalloc(SIZE_MAX) == NULL);
    assert(kmalloc(SIZE_MAX - 7) == NULL);
    assert(kmalloc(SIZE_MAX - 15) == NULL);
    assert(kcalloc(SIZE_MAX / 2 + 1, 2) == NULL);
    assert(kcalloc(1024, SIZE_MAX / 100) == NULL);
    assert(heap_validate());

    printf("  PASS: Alignment >= 16 bytes and arithmetic overflow rejection verified.\n");
}

static void test_splitting_and_exact_fit(void) {
    printf("[HOST TEST] 3. Exact fit and minimum split boundaries...\n");
    setup_fresh_arena();

    struct heap_stats s0;
    heap_get_stats(&s0);
    size_t root_cap = s0.free_bytes;

    /* Exact fit: request root_cap. No room for header + min_alloc, so entire block should be used */
    void *p_exact = kmalloc(root_cap);
    assert(p_exact != NULL);
    struct heap_stats s_exact;
    heap_get_stats(&s_exact);
    assert(s_exact.free_blocks == 0);
    assert(s_exact.free_bytes == 0);
    assert(s_exact.live_allocations == 1);
    assert(heap_validate());
    kfree(p_exact);
    assert(heap_validate());

    /* Near-exact fit: request root_cap - 32. Leftover is 32 bytes (< sizeof(struct heap_block) + 16 = 80).
     * Cannot split! Must take entire block. */
    void *p_near = kmalloc(root_cap - 32);
    assert(p_near != NULL);
    struct heap_stats s_near;
    heap_get_stats(&s_near);
    assert(s_near.free_blocks == 0);
    assert(s_near.free_bytes == 0);
    assert(s_near.live_allocations == 1);
    assert(heap_validate());
    kfree(p_near);
    assert(heap_validate());

    /* Split boundary: request root_cap - 80. Leftover is exactly 80 bytes (64 header + 16 payload).
     * Must cleanly split! Exactly 1 free block with capacity 16. */
    void *p_split = kmalloc(root_cap - 80);
    assert(p_split != NULL);
    struct heap_stats s_split;
    heap_get_stats(&s_split);
    assert(s_split.free_blocks == 1);
    assert(s_split.free_bytes == 16);
    assert(s_split.live_allocations == 1);
    assert(heap_validate());

    /* Now allocate that tiny 16-byte remainder */
    void *p_tiny = kmalloc(16);
    assert(p_tiny != NULL);
    struct heap_stats s_full;
    heap_get_stats(&s_full);
    assert(s_full.free_blocks == 0);
    assert(s_full.live_allocations == 2);
    assert(heap_validate());

    kfree(p_split);
    kfree(p_tiny);
    assert(heap_validate());

    struct heap_stats s_end;
    heap_get_stats(&s_end);
    assert(s_end.free_blocks == 1);
    assert(s_end.free_bytes == root_cap);

    printf("  PASS: Exact fit and 80-byte split boundaries verified.\n");
}

static void test_bidirectional_coalescing(void) {
    printf("[HOST TEST] 4. Coalescing in both directions (left, right, bridge)...\n");
    setup_fresh_arena();

    struct heap_stats s0;
    heap_get_stats(&s0);

    /* Allocate three consecutive blocks A, B, C */
    void *a = kmalloc(64);
    void *b = kmalloc(64);
    void *c = kmalloc(64);
    assert(a && b && c);
    assert(heap_validate());

    /* Right coalescing: free B, then free A -> A coalesces with B */
    kfree(b);
    assert(heap_validate());
    kfree(a);
    assert(heap_validate());
    kfree(c);
    assert(heap_validate());

    struct heap_stats s1;
    heap_get_stats(&s1);
    assert(s1.free_blocks == 1);
    assert(s1.free_bytes == s0.free_bytes);

    /* Left coalescing: allocate A, B, C; free A, then free B -> B coalesces into A */
    a = kmalloc(64);
    b = kmalloc(64);
    c = kmalloc(64);
    assert(a && b && c);
    kfree(a);
    assert(heap_validate());
    kfree(b); /* Coalesces into A */
    assert(heap_validate());
    kfree(c);
    assert(heap_validate());

    struct heap_stats s2;
    heap_get_stats(&s2);
    assert(s2.free_blocks == 1);
    assert(s2.free_bytes == s0.free_bytes);

    /* Bridge coalescing: allocate A, B, C; free A, free C; then free B -> coalesces (A + B + C) */
    a = kmalloc(64);
    b = kmalloc(64);
    c = kmalloc(64);
    assert(a && b && c);
    kfree(a);
    assert(heap_validate());
    kfree(c);
    assert(heap_validate());
    kfree(b); /* Bridges A and C */
    assert(heap_validate());

    struct heap_stats s3;
    heap_get_stats(&s3);
    assert(s3.free_blocks == 1);
    assert(s3.free_bytes == s0.free_bytes);

    printf("  PASS: Bidirectional and bridge coalescing fully verified.\n");
}

static void test_calloc_zeroing(void) {
    printf("[HOST TEST] 5. kcalloc zeroing and payload cleanliness...\n");
    setup_fresh_arena();

    /* Dirty the memory first */
    void *p = kmalloc(256);
    assert(p != NULL);
    memset(p, 0xEE, 256);
    kfree(p);
    assert(heap_validate());

    /* Now allocate with kcalloc */
    uint8_t *zc = (uint8_t *)kcalloc(16, 16);
    assert(zc != NULL);
    for (size_t i = 0; i < 256; i++) {
        assert(zc[i] == 0);
    }
    assert(heap_validate());
    kfree(zc);
    assert(heap_validate());

    printf("  PASS: kcalloc zeroing verified.\n");
}

static void test_realloc_behavior(void) {
    printf("[HOST TEST] 6. krealloc shrink, grow, move, and failure preservation...\n");
    setup_fresh_arena();

    /* 6a. Shrink */
    uint8_t *p = (uint8_t *)kmalloc(512);
    assert(p != NULL);
    for (size_t i = 0; i < 512; i++) p[i] = (uint8_t)(i & 0xFF);

    uint8_t *p_shrunk = (uint8_t *)krealloc(p, 128);
    assert(p_shrunk == p); /* In-place shrink */
    for (size_t i = 0; i < 128; i++) {
        assert(p_shrunk[i] == (uint8_t)(i & 0xFF));
    }
    assert(heap_validate());

    /* 6b. Grow in place (forward coalesce) */
    uint8_t *p_grown = (uint8_t *)krealloc(p_shrunk, 256);
    assert(p_grown == p); /* In-place forward coalesce */
    for (size_t i = 0; i < 128; i++) {
        assert(p_grown[i] == (uint8_t)(i & 0xFF));
    }
    assert(heap_validate());

    /* 6c. Move (when next block is NOT free) */
    void *barrier = kmalloc(64);
    assert(barrier != NULL);

    uint8_t *p_moved = (uint8_t *)krealloc(p_grown, 1024);
    assert(p_moved != NULL);
    assert(p_moved != p); /* Must move since barrier blocks forward growth */
    for (size_t i = 0; i < 128; i++) {
        assert(p_moved[i] == (uint8_t)(i & 0xFF));
    }
    assert(heap_validate());

    /* 6d. Failure preservation: exhaust arena, then attempt impossible grow */
    void *fillers[128];
    size_t fcount = 0;
    while (fcount < 128) {
        void *f = kmalloc(512);
        if (!f) break;
        fillers[fcount++] = f;
    }
    assert(fcount > 0);

    /* Mark p_moved with specific canary */
    p_moved[0] = 0xAA;
    p_moved[127] = 0x55;

    /* Request huge size that cannot fit in exhausted arena */
    void *fail_realloc = krealloc(p_moved, 32768);
    assert(fail_realloc == NULL); /* Must fail */
    /* Original pointer and data must remain valid! */
    assert(p_moved[0] == 0xAA);
    assert(p_moved[127] == 0x55);
    assert(heap_validate());

    /* Clean up */
    kfree(p_moved);
    kfree(barrier);
    for (size_t i = 0; i < fcount; i++) kfree(fillers[i]);
    assert(heap_validate());

    struct heap_stats st;
    heap_get_stats(&st);
    assert(st.live_allocations == 0);
    assert(st.free_blocks == 1);

    printf("  PASS: krealloc shrink, grow, move, and failure preservation verified.\n");
}

static void test_exhaustion_and_recovery(void) {
    printf("[HOST TEST] 7. Exhaustion and clean recovery...\n");
    setup_fresh_arena();

    struct heap_stats s0;
    heap_get_stats(&s0);

    void *blocks[256];
    size_t count = 0;
    while (count < 256) {
        void *b = kmalloc(256);
        if (!b) break;
        memset(b, 0xCC, 256);
        blocks[count++] = b;
    }
    assert(count > 0);
    assert(kmalloc(256) == NULL); /* Full exhaustion */
    assert(heap_validate());

    /* Free every block and verify full recovery */
    for (size_t i = 0; i < count; i++) {
        kfree(blocks[i]);
    }
    assert(heap_validate());

    struct heap_stats s_end;
    heap_get_stats(&s_end);
    assert(s_end.free_blocks == 1);
    assert(s_end.free_bytes == s0.free_bytes);
    assert(s_end.live_allocations == 0);

    printf("  PASS: Exhaustion and recovery verified (%zu blocks allocated & restored).\n", count);
}

static void test_invalid_frees_isolated(void) {
    printf("[HOST TEST] 8. Invalid, interior, and double-free detection via panic traps...\n");
    setup_fresh_arena();

    void *valid = kmalloc(64);
    assert(valid != NULL);

    /* 8a. Free pointer outside arena */
    g_expect_panic = true;
    int c1 = g_panics_caught;
    if (setjmp(g_panic_jmp) == 0) {
        kfree((void *)0x1000);
        assert(false && "Did not panic on pointer outside arena!");
    }
    assert(g_panics_caught == c1 + 1);

    /* 8b. Free unaligned pointer */
    int c2 = g_panics_caught;
    if (setjmp(g_panic_jmp) == 0) {
        kfree((void *)((uintptr_t)valid + 1));
        assert(false && "Did not panic on unaligned pointer!");
    }
    assert(g_panics_caught == c2 + 1);

    /* 8c. Free interior pointer */
    int c3 = g_panics_caught;
    if (setjmp(g_panic_jmp) == 0) {
        kfree((void *)((uintptr_t)valid + 16));
        assert(false && "Did not panic on interior pointer!");
    }
    assert(g_panics_caught == c3 + 1);

    /* 8d. Valid free followed by double-free */
    g_expect_panic = false;
    kfree(valid);
    assert(heap_validate());

    g_expect_panic = true;
    int c4 = g_panics_caught;
    if (setjmp(g_panic_jmp) == 0) {
        kfree(valid); /* Double-free */
        assert(false && "Did not panic on double-free!");
    }
    assert(g_panics_caught == c4 + 1);

    g_expect_panic = false;
    printf("  PASS: Out-of-bounds, unaligned, interior, and double-free panics correctly caught.\n");
}

/* Pseudo-random number generator for deterministic fuzzing */
static uint32_t g_rand_state = 0x12345678;
static uint32_t xorshift32(void) {
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

#define FUZZ_MAX_LIVE 128
struct live_slot {
    void *ptr;
    size_t size;
    uint8_t fill_byte;
};

static void test_seeded_random_sequences(void) {
    printf("[HOST TEST] 9. Seeded randomized sequences with payload-integrity checks (10,000 iterations)...\n");
    setup_fresh_arena();

    struct heap_stats s0;
    heap_get_stats(&s0);

    struct live_slot live[FUZZ_MAX_LIVE];
    memset(live, 0, sizeof(live));
    size_t live_count = 0;

    g_rand_state = 0xCAFEBABE; /* Deterministic fixed seed */

    for (int iter = 0; iter < 10000; iter++) {
        uint32_t action = xorshift32() % 100;

        if (action < 45) {
            /* 45% Malloc */
            if (live_count < FUZZ_MAX_LIVE) {
                size_t sz = (xorshift32() % 512) + 1;
                uint8_t byte = (uint8_t)(xorshift32() & 0xFF);
                void *p = kmalloc(sz);
                if (p) {
                    assert(((uintptr_t)p % HEAP_ALIGNMENT) == 0);
                    memset(p, byte, sz);
                    for (size_t s = 0; s < FUZZ_MAX_LIVE; s++) {
                        if (!live[s].ptr) {
                            live[s].ptr = p;
                            live[s].size = sz;
                            live[s].fill_byte = byte;
                            live_count++;
                            break;
                        }
                    }
                }
            }
        } else if (action < 80) {
            /* 35% Free */
            if (live_count > 0) {
                size_t target_idx = xorshift32() % FUZZ_MAX_LIVE;
                for (size_t s = 0; s < FUZZ_MAX_LIVE; s++) {
                    size_t idx = (target_idx + s) % FUZZ_MAX_LIVE;
                    if (live[idx].ptr) {
                        /* Verify payload integrity before free */
                        uint8_t *bytes = (uint8_t *)live[idx].ptr;
                        for (size_t b = 0; b < live[idx].size; b++) {
                            assert(bytes[b] == live[idx].fill_byte);
                        }
                        kfree(live[idx].ptr);
                        live[idx].ptr = NULL;
                        live[idx].size = 0;
                        live_count--;
                        break;
                    }
                }
            }
        } else if (action < 90) {
            /* 10% Calloc */
            if (live_count < FUZZ_MAX_LIVE) {
                size_t num = (xorshift32() % 8) + 1;
                size_t elem = (xorshift32() % 64) + 1;
                size_t total = num * elem;
                uint8_t *p = (uint8_t *)kcalloc(num, elem);
                if (p) {
                    assert(((uintptr_t)p % HEAP_ALIGNMENT) == 0);
                    for (size_t b = 0; b < total; b++) {
                        assert(p[b] == 0);
                    }
                    uint8_t byte = (uint8_t)(xorshift32() & 0xFF);
                    memset(p, byte, total);
                    for (size_t s = 0; s < FUZZ_MAX_LIVE; s++) {
                        if (!live[s].ptr) {
                            live[s].ptr = p;
                            live[s].size = total;
                            live[s].fill_byte = byte;
                            live_count++;
                            break;
                        }
                    }
                }
            }
        } else {
            /* 10% Realloc */
            if (live_count > 0) {
                size_t target_idx = xorshift32() % FUZZ_MAX_LIVE;
                for (size_t s = 0; s < FUZZ_MAX_LIVE; s++) {
                    size_t idx = (target_idx + s) % FUZZ_MAX_LIVE;
                    if (live[idx].ptr) {
                        /* Verify existing payload */
                        uint8_t *bytes = (uint8_t *)live[idx].ptr;
                        for (size_t b = 0; b < live[idx].size; b++) {
                            assert(bytes[b] == live[idx].fill_byte);
                        }

                        size_t new_sz = (xorshift32() % 1024) + 1;
                        void *re = krealloc(live[idx].ptr, new_sz);
                        if (re) {
                            assert(((uintptr_t)re % HEAP_ALIGNMENT) == 0);
                            /* Verify preserved prefix */
                            size_t min_sz = (live[idx].size < new_sz) ? live[idx].size : new_sz;
                            uint8_t *re_bytes = (uint8_t *)re;
                            for (size_t b = 0; b < min_sz; b++) {
                                assert(re_bytes[b] == live[idx].fill_byte);
                            }
                            /* Refill new buffer */
                            uint8_t new_byte = (uint8_t)(xorshift32() & 0xFF);
                            memset(re, new_byte, new_sz);
                            live[idx].ptr = re;
                            live[idx].size = new_sz;
                            live[idx].fill_byte = new_byte;
                        }
                        break;
                    }
                }
            }
        }

        if ((iter % 1000) == 0) {
            assert(heap_validate());
        }
    }

    /* Free all remaining live allocations and verify payload integrity */
    for (size_t s = 0; s < FUZZ_MAX_LIVE; s++) {
        if (live[s].ptr) {
            uint8_t *bytes = (uint8_t *)live[s].ptr;
            for (size_t b = 0; b < live[s].size; b++) {
                assert(bytes[b] == live[s].fill_byte);
            }
            kfree(live[s].ptr);
            live[s].ptr = NULL;
        }
    }

    assert(heap_validate());
    struct heap_stats s_final;
    heap_get_stats(&s_final);
    assert(s_final.live_allocations == 0);
    assert(s_final.free_blocks == 1);
    assert(s_final.free_bytes == s0.free_bytes);

    printf("  PASS: 10,000 randomized iterations passed with zero corruption and 100%% recovery.\n");
}

int main(void) {
    printf("======================================================\n");
    printf("       AstraOS Kernel Heap Allocator Host Tests       \n");
    printf("======================================================\n");

    test_zero_and_null_behavior();
    test_alignment_and_overflow();
    test_splitting_and_exact_fit();
    test_bidirectional_coalescing();
    test_calloc_zeroing();
    test_realloc_behavior();
    test_exhaustion_and_recovery();
    test_invalid_frees_isolated();
    test_seeded_random_sequences();

    printf("------------------------------------------------------\n");
    printf("[SUCCESS] All host-side Heap Allocator tests passed!\n");
    printf("======================================================\n");
    return 0;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int run_all_heap_tests(void) {
    return main();
}

