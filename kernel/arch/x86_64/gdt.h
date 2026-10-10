#ifndef ASTRA_ARCH_X86_64_GDT_H
#define ASTRA_ARCH_X86_64_GDT_H

#include <kernel/types.h>

#define GDT_KERNEL_CODE_SEG 0x08
#define GDT_KERNEL_DATA_SEG 0x10
#define GDT_USER_DATA_SEG   0x18
#define GDT_USER_CODE_SEG   0x20

void gdt_init(void);

#endif /* ASTRA_ARCH_X86_64_GDT_H */
