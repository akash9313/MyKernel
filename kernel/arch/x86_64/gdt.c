#include "gdt.h"

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

static struct gdt_entry g_gdt[5];
static struct gdt_pointer g_gdtr;

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
