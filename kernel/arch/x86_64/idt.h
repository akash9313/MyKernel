#ifndef ASTRA_ARCH_X86_64_IDT_H
#define ASTRA_ARCH_X86_64_IDT_H

#include <kernel/types.h>

struct __attribute__((packed)) interrupt_frame {
    /* Pushed by common stub */
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code;

    /* Pushed by CPU */
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

void idt_init(void);
void idt_set_gate(u8 vector, void *isr, u8 flags);
void idt_set_gate_ist(u8 vector, void *isr, u8 flags, u8 ist);
void interrupt_handler(struct interrupt_frame *frame);

#endif /* ASTRA_ARCH_X86_64_IDT_H */
