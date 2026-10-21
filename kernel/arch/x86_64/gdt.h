#ifndef ASTRA_ARCH_X86_64_GDT_H
#define ASTRA_ARCH_X86_64_GDT_H

#include <kernel/types.h>

#define GDT_KERNEL_CODE_SEG 0x08
#define GDT_KERNEL_DATA_SEG 0x10
#define GDT_USER_DATA_SEG   0x18
#define GDT_USER_CODE_SEG   0x20
#define GDT_TSS_SEG         0x28

struct __attribute__((packed)) tss {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

void gdt_init(void);
void tss_init(u64 ist1_stack_top);
void tss_set_rsp0(u64 rsp0);

#endif /* ASTRA_ARCH_X86_64_GDT_H */
