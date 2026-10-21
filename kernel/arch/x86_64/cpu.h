#ifndef ASTRA_ARCH_X86_64_CPU_H
#define ASTRA_ARCH_X86_64_CPU_H

#include <kernel/types.h>

static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(u16 port, u32 val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) {
    __asm__ volatile ("cli" : : : "memory");
}

static inline void sti(void) {
    __asm__ volatile ("sti" : : : "memory");
}

static inline void hlt(void) {
    __asm__ volatile ("hlt");
}

static inline u64 read_rflags(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    return flags;
}

static inline u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(u64 flags) {
    if (flags & (1ULL << 9)) {
        sti();
    }
}

static inline __attribute__((noreturn)) void halt_forever(void) {
    for (;;) {
        cli();
        hlt();
    }
}

static inline u64 read_cr0(void) {
    u64 val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(u64 val) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val) : "memory");
}





static inline u64 read_cr2(void) {
    u64 val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline u64 read_cr3(void) {
    u64 val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(u64 val) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline u64 read_cr4(void) {
    u64 val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(u64 val) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val) : "memory");
}

static inline void invlpg(virt_addr_t va) {
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static inline void cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
    u32 a, b, c, d;
    __asm__ volatile ("cpuid"
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(leaf), "c"(subleaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static inline u64 read_msr(u32 msr) {
    u32 low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((u64)high << 32) | low;
}

static inline void write_msr(u32 msr, u64 val) {
    u32 low = (u32)val;
    u32 high = (u32)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

#endif /* ASTRA_ARCH_X86_64_CPU_H */
