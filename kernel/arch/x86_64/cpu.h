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

static inline void halt_forever(void) {
    for (;;) {
        cli();
        hlt();
    }
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

#endif /* ASTRA_ARCH_X86_64_CPU_H */
