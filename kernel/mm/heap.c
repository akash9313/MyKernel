#include <kernel/heap.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/preempt.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef SIZE_MAX
#define SIZE_MAX (~(size_t)0)
#endif

static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

static void *kmemcpy(void *dest, const void *src, size_t len) {
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dest;
}

/* Global Heap State */
static bool g_heap_initialized = false;
static void *g_arena_base = NULL;
static size_t g_arena_size = 0;
static struct heap_block *g_free_list_head = NULL;
static int g_heap_fault_countdown = -1;

/* Bookkeeping for transactional initialization rollback */
static phys_addr_t g_heap_allocated_frames[HEAP_ARENA_PAGES];
static size_t g_heap_allocated_frames_count = 0;
static virt_addr_t g_heap_mapped_vas[HEAP_ARENA_PAGES];
static size_t g_heap_mapped_vas_count = 0;

void heap_set_fault_injection(int countdown) {
    g_heap_fault_countdown = countdown;
}

static void free_list_insert(struct heap_block *block) {
    if (!block) return;
    block->next_free = g_free_list_head;
    block->prev_free = NULL;
    if (g_free_list_head) {
        g_free_list_head->prev_free = block;
    }
    g_free_list_head = block;
}

static void free_list_remove(struct heap_block *block) {
    if (!block) return;
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        g_free_list_head = block->next_free;
    }
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    block->next_free = NULL;
    block->prev_free = NULL;
}

/* Core Allocator Initialization on an arbitrary memory buffer */
bool heap_core_init(void *arena_base, size_t arena_size) {
    if (!arena_base || arena_size < (sizeof(struct heap_block) + HEAP_MIN_ALLOC)) {
        return false;
    }
    if (((uintptr_t)arena_base % HEAP_ALIGNMENT) != 0 || (arena_size % HEAP_ALIGNMENT) != 0) {
        return false;
    }

    g_arena_base = arena_base;
    g_arena_size = arena_size;

    struct heap_block *root = (struct heap_block *)arena_base;
    root->magic = HEAP_BLOCK_MAGIC;
    root->capacity = arena_size - sizeof(struct heap_block);
    root->requested_size = 0;
    root->is_free = true;
    for (int i = 0; i < 7; i++) root->reserved[i] = 0;
    root->prev_phys = NULL;
    root->next_phys = NULL;
    root->next_free = NULL;
    root->prev_free = NULL;

    g_free_list_head = root;
    g_heap_initialized = true;
    return true;
}

static void heap_panic(const char *msg) {
    kprintf("\n[HEAP CORRUPTION] %s\n", msg);
    panic("Kernel Heap invariant violation detected!");
}

/* Core Allocation Logic */
void *heap_core_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    if (size > g_arena_size - sizeof(struct heap_block)) {
        return NULL;
    }

    /* Check arithmetic overflow when rounding up to 16 bytes */
    if (size > SIZE_MAX - (HEAP_ALIGNMENT - 1)) {
        return NULL;
    }
    size_t aligned_size = (size + (HEAP_ALIGNMENT - 1)) & ~(HEAP_ALIGNMENT - 1);
    if (aligned_size < HEAP_MIN_ALLOC) {
        aligned_size = HEAP_MIN_ALLOC;
    }

    /* First-fit search */
    struct heap_block *curr = g_free_list_head;
    while (curr) {
        if (curr->magic != HEAP_BLOCK_MAGIC) {
            heap_panic("Corrupted block magic encountered during free-list traversal!");
        }
        if (curr->capacity >= aligned_size) {
            break;
        }
        curr = curr->next_free;
    }

    if (!curr) {
        return NULL; /* Out of memory in fixed arena */
    }

    size_t leftover = curr->capacity - aligned_size;
    /* Split if leftover can host a block header and minimum payload */
    if (leftover >= sizeof(struct heap_block) + HEAP_MIN_ALLOC) {
        struct heap_block *split_block = (struct heap_block *)((uintptr_t)curr + sizeof(struct heap_block) + aligned_size);
        split_block->magic = HEAP_BLOCK_MAGIC;
        split_block->capacity = leftover - sizeof(struct heap_block);
        split_block->requested_size = 0;
        split_block->is_free = true;
        for (int i = 0; i < 7; i++) split_block->reserved[i] = 0;

        /* Splice physical adjacency links */
        split_block->prev_phys = curr;
        split_block->next_phys = curr->next_phys;
        if (curr->next_phys) {
            curr->next_phys->prev_phys = split_block;
        }
        curr->next_phys = split_block;
        curr->capacity = aligned_size;

        /* Replace curr with split_block in free list */
        split_block->prev_free = curr->prev_free;
        split_block->next_free = curr->next_free;
        if (curr->prev_free) {
            curr->prev_free->next_free = split_block;
        } else {
            g_free_list_head = split_block;
        }
        if (curr->next_free) {
            curr->next_free->prev_free = split_block;
        }
        curr->next_free = NULL;
        curr->prev_free = NULL;
    } else {
        /* Allocate full block without splitting */
        free_list_remove(curr);
    }

    curr->is_free = false;
    curr->requested_size = size;

    return (void *)((uintptr_t)curr + sizeof(struct heap_block));
}

void heap_core_free(void *ptr) {
    if (!ptr) {
        return;
    }

    /* 1. Range check within arena */
    uintptr_t uptr = (uintptr_t)ptr;
    uintptr_t arena_start = (uintptr_t)g_arena_base;
    uintptr_t arena_end = arena_start + g_arena_size;

    if (uptr < arena_start + sizeof(struct heap_block) || uptr >= arena_end) {
        heap_panic("kfree: pointer outside heap arena!");
    }

    /* 2. Alignment check */
    if ((uptr % HEAP_ALIGNMENT) != 0) {
        heap_panic("kfree: unaligned pointer!");
    }

    /* 3. Header check */
    struct heap_block *block = (struct heap_block *)(uptr - sizeof(struct heap_block));
    if (block->magic != HEAP_BLOCK_MAGIC) {
        heap_panic("kfree: invalid block magic (corrupted header or invalid pointer)!");
    }
    if (block->is_free) {
        heap_panic("kfree: double-free detected!");
    }

    /* Verify physical link integrity */
    if (block->prev_phys && block->prev_phys->next_phys != block) {
        heap_panic("kfree: broken prev_phys linkage!");
    }
    if (block->next_phys && block->next_phys->prev_phys != block) {
        heap_panic("kfree: broken next_phys linkage!");
    }

    block->is_free = true;
    block->requested_size = 0;

    /* 4. Coalesce right if next_phys is free */
    if (block->next_phys && block->next_phys->is_free) {
        struct heap_block *next = block->next_phys;
        free_list_remove(next);
        block->capacity += sizeof(struct heap_block) + next->capacity;
        block->next_phys = next->next_phys;
        if (next->next_phys) {
            next->next_phys->prev_phys = block;
        }
        next->magic = 0; /* Invalidate victim magic */
    }

    /* 5. Coalesce left if prev_phys is free */
    if (block->prev_phys && block->prev_phys->is_free) {
        struct heap_block *prev = block->prev_phys;
        prev->capacity += sizeof(struct heap_block) + block->capacity;
        prev->next_phys = block->next_phys;
        if (block->next_phys) {
            block->next_phys->prev_phys = prev;
        }
        block->magic = 0; /* Invalidate victim magic */
    } else {
        /* Not coalesced into prev_phys: insert into free list */
        free_list_insert(block);
    }
}

void *heap_core_calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = heap_core_malloc(total);
    if (ptr) {
        kmemset(ptr, 0, total);
    }
    return ptr;
}

void *heap_core_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return heap_core_malloc(size);
    }
    if (size == 0) {
        heap_core_free(ptr);
        return NULL;
    }

    uintptr_t uptr = (uintptr_t)ptr;
    uintptr_t arena_start = (uintptr_t)g_arena_base;
    uintptr_t arena_end = arena_start + g_arena_size;

    if (uptr < arena_start + sizeof(struct heap_block) || uptr >= arena_end || (uptr % HEAP_ALIGNMENT) != 0) {
        heap_panic("krealloc: invalid pointer!");
    }

    struct heap_block *block = (struct heap_block *)(uptr - sizeof(struct heap_block));
    if (block->magic != HEAP_BLOCK_MAGIC || block->is_free) {
        heap_panic("krealloc: invalid block magic or realloc of freed block!");
    }

    if (size > SIZE_MAX - (HEAP_ALIGNMENT - 1)) {
        return NULL;
    }
    size_t aligned_size = (size + (HEAP_ALIGNMENT - 1)) & ~(HEAP_ALIGNMENT - 1);
    if (aligned_size < HEAP_MIN_ALLOC) {
        aligned_size = HEAP_MIN_ALLOC;
    }

    /* Case A: Shrink in place */
    if (aligned_size <= block->capacity) {
        size_t leftover = block->capacity - aligned_size;
        if (leftover >= sizeof(struct heap_block) + HEAP_MIN_ALLOC) {
            struct heap_block *split = (struct heap_block *)((uintptr_t)block + sizeof(struct heap_block) + aligned_size);
            split->magic = HEAP_BLOCK_MAGIC;
            split->capacity = leftover - sizeof(struct heap_block);
            split->requested_size = 0;
            split->is_free = true;
            for (int i = 0; i < 7; i++) split->reserved[i] = 0;

            split->prev_phys = block;
            split->next_phys = block->next_phys;
            if (block->next_phys) {
                block->next_phys->prev_phys = split;
            }
            block->next_phys = split;
            block->capacity = aligned_size;

            /* Coalesce split with next_phys if free */
            if (split->next_phys && split->next_phys->is_free) {
                struct heap_block *v = split->next_phys;
                free_list_remove(v);
                split->capacity += sizeof(struct heap_block) + v->capacity;
                split->next_phys = v->next_phys;
                if (v->next_phys) {
                    v->next_phys->prev_phys = split;
                }
                v->magic = 0;
            }
            free_list_insert(split);
        }
        block->requested_size = size;
        return ptr;
    }

    /* Case B: Grow in place by coalescing forward with free next_phys */
    if (block->next_phys && block->next_phys->is_free) {
        size_t combined = block->capacity + sizeof(struct heap_block) + block->next_phys->capacity;
        if (combined >= aligned_size) {
            struct heap_block *v = block->next_phys;
            free_list_remove(v);
            block->capacity = combined;
            block->next_phys = v->next_phys;
            if (v->next_phys) {
                v->next_phys->prev_phys = block;
            }
            v->magic = 0;

            /* Split leftover excess if large enough */
            size_t excess = block->capacity - aligned_size;
            if (excess >= sizeof(struct heap_block) + HEAP_MIN_ALLOC) {
                struct heap_block *sp = (struct heap_block *)((uintptr_t)block + sizeof(struct heap_block) + aligned_size);
                sp->magic = HEAP_BLOCK_MAGIC;
                sp->capacity = excess - sizeof(struct heap_block);
                sp->requested_size = 0;
                sp->is_free = true;
                for (int i = 0; i < 7; i++) sp->reserved[i] = 0;

                sp->prev_phys = block;
                sp->next_phys = block->next_phys;
                if (block->next_phys) {
                    block->next_phys->prev_phys = sp;
                }
                block->next_phys = sp;
                block->capacity = aligned_size;
                free_list_insert(sp);
            }

            block->requested_size = size;
            return ptr;
        }
    }

    /* Case C: Allocate new block, copy min(old, new), and free old */
    void *new_ptr = heap_core_malloc(size);
    if (!new_ptr) {
        return NULL; /* Failure preserves original allocation and data! */
    }
    size_t copy_bytes = (block->requested_size < size) ? block->requested_size : size;
    kmemcpy(new_ptr, ptr, copy_bytes);
    heap_core_free(ptr);
    return new_ptr;
}

bool heap_core_validate(void) {
    if (!g_arena_base || g_arena_size == 0) {
        return false;
    }

    size_t total_covered = 0;
    size_t free_phys_count = 0;
    struct heap_block *curr = (struct heap_block *)g_arena_base;

    while (curr) {
        if (curr->magic != HEAP_BLOCK_MAGIC) {
            return false;
        }
        if ((uintptr_t)curr < (uintptr_t)g_arena_base ||
            (uintptr_t)curr >= (uintptr_t)g_arena_base + g_arena_size) {
            return false;
        }
        if (curr->capacity < HEAP_MIN_ALLOC || (curr->capacity % HEAP_ALIGNMENT) != 0) {
            return false;
        }
        if (curr->next_phys) {
            if (curr->next_phys->prev_phys != curr) return false;
            if ((uintptr_t)curr + sizeof(struct heap_block) + curr->capacity != (uintptr_t)curr->next_phys) {
                return false;
            }
        }

        /* Coalescing invariant: no two adjacent physical blocks can both be free */
        if (curr->is_free && curr->next_phys && curr->next_phys->is_free) {
            return false;
        }

        if (curr->is_free) {
            if (curr->requested_size != 0) return false;
            free_phys_count++;
        } else {
            if (curr->requested_size == 0 || curr->requested_size > curr->capacity) return false;
        }

        total_covered += sizeof(struct heap_block) + curr->capacity;
        curr = curr->next_phys;
    }

    if (total_covered != g_arena_size) {
        return false;
    }

    /* Free-list consistency check */
    size_t free_list_count = 0;
    struct heap_block *fb = g_free_list_head;
    size_t max_blocks = g_arena_size / sizeof(struct heap_block);

    while (fb) {
        if (fb->magic != HEAP_BLOCK_MAGIC || !fb->is_free) {
            return false;
        }
        if (fb->next_free && fb->next_free->prev_free != fb) {
            return false;
        }
        free_list_count++;
        if (free_list_count > max_blocks) {
            return false; /* Cycle detection */
        }
        fb = fb->next_free;
    }

    if (free_phys_count != free_list_count) {
        return false;
    }

    return true;
}

void heap_core_get_stats(struct heap_stats *out_stats) {
    if (!out_stats) return;
    out_stats->arena_size = g_arena_size;
    out_stats->total_capacity = 0;
    out_stats->allocated_bytes = 0;
    out_stats->free_bytes = 0;
    out_stats->total_blocks = 0;
    out_stats->free_blocks = 0;
    out_stats->live_allocations = 0;

    struct heap_block *curr = (struct heap_block *)g_arena_base;
    while (curr && (uintptr_t)curr < (uintptr_t)g_arena_base + g_arena_size) {
        if (curr->magic != HEAP_BLOCK_MAGIC) break;
        out_stats->total_blocks++;
        out_stats->total_capacity += curr->capacity;
        if (curr->is_free) {
            out_stats->free_blocks++;
            out_stats->free_bytes += curr->capacity;
        } else {
            out_stats->live_allocations++;
            out_stats->allocated_bytes += curr->requested_size;
        }
        curr = curr->next_phys;
    }
}

/* Public Kernel Heap API Wrappers */

void *kmalloc(size_t size) {
    if (!g_heap_initialized) return NULL;
    if (in_interrupt()) panic("kmalloc forbidden in interrupt context!");
    preempt_disable();
    void *ptr = heap_core_malloc(size);
    preempt_enable();
    return ptr;
}

void kfree(void *ptr) {
    if (!g_heap_initialized || !ptr) return;
    if (in_interrupt()) panic("kfree forbidden in interrupt context!");
    preempt_disable();
    heap_core_free(ptr);
    preempt_enable();
}

void *kcalloc(size_t count, size_t size) {
    if (!g_heap_initialized) return NULL;
    if (in_interrupt()) panic("kcalloc forbidden in interrupt context!");
    preempt_disable();
    void *ptr = heap_core_calloc(count, size);
    preempt_enable();
    return ptr;
}

void *krealloc(void *ptr, size_t size) {
    if (!g_heap_initialized) return NULL;
    if (in_interrupt()) panic("krealloc forbidden in interrupt context!");
    preempt_disable();
    void *new_ptr = heap_core_realloc(ptr, size);
    preempt_enable();
    return new_ptr;
}

bool heap_validate(void) {
    if (!g_heap_initialized) return false;
    return heap_core_validate();
}

void heap_get_stats(struct heap_stats *out_stats) {
    heap_core_get_stats(out_stats);
}

bool heap_is_initialized(void) {
    return g_heap_initialized;
}

void heap_print_stats(void) {
    struct heap_stats st;
    heap_get_stats(&st);
    kprintf("======================= Kernel Heap Stats =======================\n");
    kprintf("Arena Range:       0x%016llx .. 0x%016llx (Size: %llu KiB)\n",
            (unsigned long long)HEAP_START_VA, (unsigned long long)HEAP_END_VA,
            (unsigned long long)(st.arena_size / 1024ULL));
    kprintf("Lower Guard Page:  0x%016llx (Unmapped)\n", (unsigned long long)HEAP_LOWER_GUARD_VA);
    kprintf("Upper Guard Page:  0x%016llx (Unmapped)\n", (unsigned long long)HEAP_UPPER_GUARD_VA);
    kprintf("Total Capacity:    %llu bytes (%llu KiB)\n",
            (unsigned long long)st.total_capacity, (unsigned long long)(st.total_capacity / 1024ULL));
    kprintf("Allocated Bytes:   %llu bytes (Live Allocs: %llu)\n",
            (unsigned long long)st.allocated_bytes, (unsigned long long)st.live_allocations);
    kprintf("Free Bytes:        %llu bytes (Free Blocks: %llu)\n",
            (unsigned long long)st.free_bytes, (unsigned long long)st.free_blocks);
    kprintf("Total Blocks:      %llu blocks\n", (unsigned long long)st.total_blocks);
    kprintf("=================================================================\n");
}

/* Transactional Initialization with Rollback */
int heap_init(void) {
    if (g_heap_initialized) {
        return 0;
    }

    g_heap_allocated_frames_count = 0;
    g_heap_mapped_vas_count = 0;

    u64 cr3 = read_cr3();
    phys_addr_t pml4_phys = cr3 & CR3_ADDR_MASK;

    /* 1. Eagerly allocate PMM frames for arena */
    for (size_t i = 0; i < HEAP_ARENA_PAGES; i++) {
        if (g_heap_fault_countdown == 0) {
            /* Fault injection triggered */
            goto rollback;
        }
        if (g_heap_fault_countdown > 0) {
            g_heap_fault_countdown--;
        }

        phys_addr_t frame = 0;
        if (pmm_alloc_page(&frame) != PMM_SUCCESS) {
            goto rollback;
        }
        g_heap_allocated_frames[g_heap_allocated_frames_count++] = frame;

        virt_addr_t va = HEAP_START_VA + (i * HEAP_PAGE_SIZE);
        int map_res = vmm_map_page(pml4_phys, va, frame, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC);
        if (map_res != VMM_SUCCESS) {
            goto rollback;
        }
        g_heap_mapped_vas[g_heap_mapped_vas_count++] = va;
    }

    /* 2. Initialize arena memory structure */
    if (!heap_core_init((void *)HEAP_START_VA, HEAP_ARENA_SIZE)) {
        goto rollback;
    }

    g_heap_initialized = true;
    kprintf("[HEAP] Kernel heap initialized: 64 KiB arena at [0x%016llx .. 0x%016llx]\n",
            (unsigned long long)HEAP_START_VA, (unsigned long long)HEAP_END_VA);
    kprintf("[HEAP] Boundary guard pages: Lower=0x%016llx, Upper=0x%016llx (Unmapped)\n",
            (unsigned long long)HEAP_LOWER_GUARD_VA, (unsigned long long)HEAP_UPPER_GUARD_VA);
    return 0;

rollback:
    /* Clean transactional rollback */
    for (size_t i = 0; i < g_heap_mapped_vas_count; i++) {
        vmm_unmap_page(pml4_phys, g_heap_mapped_vas[i]);
        invlpg(g_heap_mapped_vas[i]);
    }
    for (size_t i = 0; i < g_heap_allocated_frames_count; i++) {
        pmm_free_page(g_heap_allocated_frames[i]);
    }
    g_heap_allocated_frames_count = 0;
    g_heap_mapped_vas_count = 0;
    g_heap_initialized = false;
    return -1;
}

/* In-Kernel Self-Test Suite */
bool heap_self_test(void) {
    kprintf("[HEAP] Starting Kernel Heap Self-Test Suite...\n");

    /* 1. Initial State Validation */
    if (!heap_validate()) {
        kprintf("[HEAP] FAIL: Validation failed on fresh heap!\n");
        return false;
    }

    struct heap_stats s0;
    heap_get_stats(&s0);
    if (s0.live_allocations != 0 || s0.free_blocks != 1) {
        kprintf("[HEAP] FAIL: Initial state mismatch: live=%llu, free_blocks=%llu\n",
                (unsigned long long)s0.live_allocations, (unsigned long long)s0.free_blocks);
        return false;
    }

    /* 2. Zero-size and NULL free behavior */
    if (kmalloc(0) != NULL) {
        kprintf("[HEAP] FAIL: kmalloc(0) did not return NULL!\n");
        return false;
    }
    kfree(NULL); /* Must be a safe no-op */

    /* 3. Basic Allocation & Alignment */
    void *p1 = kmalloc(24);
    if (!p1 || ((uintptr_t)p1 % HEAP_ALIGNMENT) != 0) {
        kprintf("[HEAP] FAIL: p1 unaligned or NULL!\n");
        return false;
    }

    void *p2 = kmalloc(64);
    if (!p2 || ((uintptr_t)p2 % HEAP_ALIGNMENT) != 0) {
        kprintf("[HEAP] FAIL: p2 unaligned or NULL!\n");
        return false;
    }

    void *p3 = kmalloc(128);
    if (!p3 || ((uintptr_t)p3 % HEAP_ALIGNMENT) != 0) {
        kprintf("[HEAP] FAIL: p3 unaligned or NULL!\n");
        return false;
    }

    /* 4. Payload Integrity */
    u8 *b1 = (u8 *)p1;
    for (size_t i = 0; i < 24; i++) b1[i] = (u8)(0xA0 + i);
    u8 *b2 = (u8 *)p2;
    for (size_t i = 0; i < 64; i++) b2[i] = (u8)(0xB0 + i);
    u8 *b3 = (u8 *)p3;
    for (size_t i = 0; i < 128; i++) b3[i] = (u8)(0xC0 + i);

    for (size_t i = 0; i < 24; i++) {
        if (b1[i] != (u8)(0xA0 + i)) { kprintf("[HEAP] FAIL: Data corruption in p1!\n"); return false; }
    }
    for (size_t i = 0; i < 64; i++) {
        if (b2[i] != (u8)(0xB0 + i)) { kprintf("[HEAP] FAIL: Data corruption in p2!\n"); return false; }
    }
    for (size_t i = 0; i < 128; i++) {
        if (b3[i] != (u8)(0xC0 + i)) { kprintf("[HEAP] FAIL: Data corruption in p3!\n"); return false; }
    }

    if (!heap_validate()) {
        kprintf("[HEAP] FAIL: Heap validation failed after basic allocs!\n");
        return false;
    }

    /* 5. kcalloc zeroing */
    u32 *calloc_arr = (u32 *)kcalloc(32, sizeof(u32));
    if (!calloc_arr) {
        kprintf("[HEAP] FAIL: kcalloc returned NULL!\n");
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        if (calloc_arr[i] != 0) {
            kprintf("[HEAP] FAIL: kcalloc memory was not zeroed!\n");
            return false;
        }
    }
    kfree(calloc_arr);

    /* 6. krealloc shrink, grow in-place, and move */
    void *re_p = kmalloc(32);
    if (!re_p) return false;
    kmemset(re_p, 0x77, 32);

    /* Shrink to 16 bytes */
    re_p = krealloc(re_p, 16);
    if (!re_p) return false;
    for (size_t i = 0; i < 16; i++) {
        if (((u8 *)re_p)[i] != 0x77) { kprintf("[HEAP] FAIL: realloc shrink data lost!\n"); return false; }
    }

    /* Grow to 256 bytes (move to larger block) */
    re_p = krealloc(re_p, 256);
    if (!re_p) return false;
    for (size_t i = 0; i < 16; i++) {
        if (((u8 *)re_p)[i] != 0x77) { kprintf("[HEAP] FAIL: realloc grow data lost!\n"); return false; }
    }
    kfree(re_p);

    /* 7. Coalescing verification in both directions */
    kfree(p2); /* Free middle block */
    if (!heap_validate()) return false;

    kfree(p1); /* Free left block -> coalesces p1 + p2 */
    if (!heap_validate()) return false;

    kfree(p3); /* Free right block -> coalesces (p1+p2) + p3 */
    if (!heap_validate()) return false;

    /* Verify full coalescence */
    struct heap_stats s_coalesced;
    heap_get_stats(&s_coalesced);
    if (s_coalesced.live_allocations != 0 || s_coalesced.free_blocks != 1 ||
        s_coalesced.free_bytes != s0.free_bytes) {
        kprintf("[HEAP] FAIL: Coalescing did not restore full arena capacity: free_bytes=%llu (expected %llu)\n",
                (unsigned long long)s_coalesced.free_bytes, (unsigned long long)s0.free_bytes);
        return false;
    }

    /* 8. Exhaustion & Recovery */
    void *ptrs[128];
    size_t alloc_count = 0;
    while (alloc_count < 128) {
        void *p = kmalloc(1024);
        if (!p) break;
        ptrs[alloc_count++] = p;
    }
    if (alloc_count == 0) {
        kprintf("[HEAP] FAIL: Could not allocate any 1 KiB blocks!\n");
        return false;
    }
    /* Exhaustion reached cleanly without corruption */
    if (!heap_validate()) return false;

    /* Free all and verify full recovery */
    for (size_t i = 0; i < alloc_count; i++) {
        kfree(ptrs[i]);
    }
    if (!heap_validate()) return false;

    heap_get_stats(&s_coalesced);
    if (s_coalesced.free_bytes != s0.free_bytes || s_coalesced.free_blocks != 1) {
        kprintf("[HEAP] FAIL: Recovery after exhaustion did not restore full arena!\n");
        return false;
    }

    kprintf("[HEAP] Self-test passed: zero/null checks, alignment, calloc, realloc, bidirectional coalescing, and recovery verified.\n");
    return true;
}
