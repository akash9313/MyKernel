#ifndef ASTRA_KERNEL_HEAP_H
#define ASTRA_KERNEL_HEAP_H

#include <kernel/types.h>

#define HEAP_ARENA_SIZE          (64ULL * 1024ULL)           /* 64 KiB fixed arena */
#define HEAP_PAGE_SIZE           4096ULL
#define HEAP_ARENA_PAGES         (HEAP_ARENA_SIZE / HEAP_PAGE_SIZE) /* 16 pages */

#define HEAP_LOWER_GUARD_VA      0xffffffff80600000ULL
#define HEAP_START_VA            0xffffffff80601000ULL
#define HEAP_END_VA              (HEAP_START_VA + HEAP_ARENA_SIZE) /* 0xffffffff80611000 */
#define HEAP_UPPER_GUARD_VA      0xffffffff80611000ULL

#define HEAP_BLOCK_MAGIC         0xA578A05B10C40001ULL
#define HEAP_ALIGNMENT           16ULL
#define HEAP_MIN_ALLOC           16ULL

struct heap_block {
    u64 magic;                    /* HEAP_BLOCK_MAGIC */
    size_t capacity;              /* Usable payload size in bytes (multiple of 16) */
    size_t requested_size;        /* User-requested size in bytes (0 if free) */
    bool is_free;                 /* True if block is free */
    u8 reserved[7];               /* Padding to preserve 64-byte total size */
    struct heap_block *prev_phys; /* Adjacent physical block lower in virtual address */
    struct heap_block *next_phys; /* Adjacent physical block higher in virtual address */
    struct heap_block *next_free; /* Next block in free list */
    struct heap_block *prev_free; /* Previous block in free list */
};

struct heap_stats {
    size_t arena_size;
    size_t total_capacity;
    size_t allocated_bytes;
    size_t free_bytes;
    size_t total_blocks;
    size_t free_blocks;
    size_t live_allocations;
};

/* Core Heap API */
int heap_init(void);
bool heap_is_initialized(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);

/* Integrity Verification & Diagnostics */
bool heap_validate(void);
void heap_get_stats(struct heap_stats *out_stats);
void heap_print_stats(void);

/* Test-only Fault Injection */
void heap_set_fault_injection(int countdown);

/* In-Kernel Self-Test */
bool heap_self_test(void);

/* Low-level Core Allocator (Host-Testable on arbitrary buffers) */
bool heap_core_init(void *arena_base, size_t arena_size);
void *heap_core_malloc(size_t size);
void heap_core_free(void *ptr);
void *heap_core_calloc(size_t count, size_t size);
void *heap_core_realloc(void *ptr, size_t size);
bool heap_core_validate(void);
void heap_core_get_stats(struct heap_stats *out_stats);

#endif /* ASTRA_KERNEL_HEAP_H */
