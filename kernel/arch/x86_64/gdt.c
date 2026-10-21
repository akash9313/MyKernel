#include "gdt.h"
#include <kernel/kprintf.h>
#include <kernel/panic.h>

struct __attribute__((packed)) gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_middle;
    u8  access;
    u8  granularity;
    u8  base_high;
};

struct __attribute__((packed)) gdt_pointer {
    u16 limit;
    u64 base;
};

static struct gdt_entry g_gdt[7];
static struct gdt_pointer g_gdtr;
static struct tss g_tss;

static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

static void gdt_set_gate(int index, u32 base, u32 limit, u8 access, u8 gran) {
    g_gdt[index].base_low = (base & 0xFFFF);
    g_gdt[index].base_middle = ((base >> 16) & 0xFF);
    g_gdt[index].base_high = ((base >> 24) & 0xFF);

    g_gdt[index].limit_low = (limit & 0xFFFF);
    g_gdt[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    g_gdt[index].access = access;
}

void gdt_init(void) {
    /* 0x00: Null segment */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* 0x08: Kernel 64-bit code segment: present, ring 0, code, readable, long mode (L=1) */
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xAF);

    /* 0x10: Kernel 64-bit data segment: present, ring 0, data, writable */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xCF);

    /* 0x18: User 64-bit data segment: present, ring 3, data, writable */
    gdt_set_gate(3, 0, 0xFFFFF, 0xF2, 0xCF);

    /* 0x20: User 64-bit code segment: present, ring 3, code, readable, long mode (L=1) */
    gdt_set_gate(4, 0, 0xFFFFF, 0xFA, 0xAF);

    /* 0x28: TSS (16 bytes, entries 5 & 6) initialized to 0 until tss_init */
    gdt_set_gate(5, 0, 0, 0, 0);
    gdt_set_gate(6, 0, 0, 0, 0);

    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base = (u64)&g_gdt;

    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        : : "m"(g_gdtr) : "rax", "memory"
    );
}

void tss_init(u64 ist1_stack_top) {
    kmemset(&g_tss, 0, sizeof(g_tss));
    g_tss.ist1 = ist1_stack_top;
    g_tss.iomap_base = sizeof(struct tss);

    u64 base = (u64)&g_tss;
    u32 limit = sizeof(struct tss) - 1;

    /* Entry 5: Low 8 bytes of 64-bit TSS descriptor */
    g_gdt[5].limit_low = (u16)(limit & 0xFFFF);
    g_gdt[5].base_low = (u16)(base & 0xFFFF);
    g_gdt[5].base_middle = (u8)((base >> 16) & 0xFF);
    g_gdt[5].access = 0x89; /* Present=1, Ring 0, 64-bit Available TSS (type 9) */
    g_gdt[5].granularity = (u8)((limit >> 16) & 0x0F);
    g_gdt[5].base_high = (u8)((base >> 24) & 0xFF);

    /* Entry 6: High 8 bytes of 64-bit TSS descriptor (Base 32..63) */
    u32 base_upper = (u32)(base >> 32);
    g_gdt[6].limit_low = (u16)(base_upper & 0xFFFF);
    g_gdt[6].base_low = (u16)((base_upper >> 16) & 0xFFFF);
    g_gdt[6].base_middle = 0;
    g_gdt[6].access = 0;
    g_gdt[6].granularity = 0;
    g_gdt[6].base_high = 0;

    /* Reload GDTR with populated TSS descriptor */
    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base = (u64)&g_gdt;
    __asm__ volatile ("lgdt %0" : : "m"(g_gdtr) : "memory");

    /* Load Task Register */
    __asm__ volatile ("ltr %0" : : "r"((u16)GDT_TSS_SEG) : "memory");

    u16 tr;
    __asm__ volatile ("str %0" : "=r"(tr));
    if (tr != GDT_TSS_SEG) {
        panic("TSS failed to load into TR! Read 0x%04x, expected 0x%04x", tr, GDT_TSS_SEG);
    }

    kprintf("[TSS] Task State Segment active (TR: 0x%04x, IST1: 0x%016llx)\n",
            tr, (unsigned long long)ist1_stack_top);
    kprintf("ASTRA_TSS_OK\n\n");
}

void tss_set_rsp0(u64 rsp0) {
    g_tss.rsp0 = rsp0;
}
