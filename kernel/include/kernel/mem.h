#ifndef ASTRA_KERNEL_MEM_H
#define ASTRA_KERNEL_MEM_H

#include <kernel/types.h>
#include <limine.h>

void memmap_init(struct limine_memmap_response *mmap);
void memmap_print(void);
u64 memmap_get_total_usable(void);
u64 memmap_get_total_memory(void);

static inline void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    while (len--) {
        *d++ = (u8)val;
    }
    return dest;
}

static inline void *kmemcpy(void *dest, const void *src, size_t len) {
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;
    while (len--) {
        *d++ = *s++;
    }
    return dest;
}

static inline int kmemcmp(const void *s1, const void *s2, size_t len) {
    const u8 *p1 = (const u8 *)s1;
    const u8 *p2 = (const u8 *)s2;
    while (len--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++;
        p2++;
    }
    return 0;
}

#endif /* ASTRA_KERNEL_MEM_H */
