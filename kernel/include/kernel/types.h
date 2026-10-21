#ifndef ASTRA_KERNEL_TYPES_H
#define ASTRA_KERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef uintptr_t virt_addr_t;
typedef uint64_t  phys_addr_t;
typedef ptrdiff_t ssize_t;

#endif /* ASTRA_KERNEL_TYPES_H */
