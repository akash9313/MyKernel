#include "idt.h"
#include <kernel/kprintf.h>
#include <kernel/arch/x86_64/cpu.h>

struct __attribute__((packed)) idt_entry {
    u16 isr_low;
    u16 kernel_cs;
    u8  ist;
    u8  attributes;
    u16 isr_mid;
    u32 isr_high;
    u32 reserved;
};

struct __attribute__((packed)) idt_pointer {
    u16 limit;
    u64 base;
};

static struct idt_entry g_idt[256];
static struct idt_pointer g_idtr;

extern void *isr_stub_0;
extern void *isr_stub_1;
extern void *isr_stub_2;
extern void *isr_stub_3;
extern void *isr_stub_4;
extern void *isr_stub_5;
extern void *isr_stub_6;
extern void *isr_stub_7;
extern void *isr_stub_8;
extern void *isr_stub_9;
extern void *isr_stub_10;
extern void *isr_stub_11;
extern void *isr_stub_12;
extern void *isr_stub_13;
extern void *isr_stub_14;
extern void *isr_stub_15;
extern void *isr_stub_16;
extern void *isr_stub_17;
extern void *isr_stub_18;
extern void *isr_stub_19;
extern void *isr_stub_20;
extern void *isr_stub_21;
extern void *isr_stub_22;
extern void *isr_stub_23;
extern void *isr_stub_24;
extern void *isr_stub_25;
extern void *isr_stub_26;
extern void *isr_stub_27;
extern void *isr_stub_28;
extern void *isr_stub_29;
extern void *isr_stub_30;
extern void *isr_stub_31;

static const char *exception_messages[32] = {
    "Division by Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point",
    "Virtualization Exception",
    "Control Protection",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection",
    "VMM Communication",
    "Security Exception",
    "Reserved"
};

void idt_set_gate(u8 vector, void *isr, u8 flags) {
    u64 addr = (u64)isr;
    g_idt[vector].isr_low = (u16)(addr & 0xFFFF);
    g_idt[vector].kernel_cs = 0x08;
    g_idt[vector].ist = 0;
    g_idt[vector].attributes = flags;
    g_idt[vector].isr_mid = (u16)((addr >> 16) & 0xFFFF);
    g_idt[vector].isr_high = (u32)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved = 0;
}

void exception_handler(struct interrupt_frame *frame) {
    cli();

    const char *name = (frame->vector < 32) ? exception_messages[frame->vector] : "User/Hardware Interrupt";

    kprintf("\n=================== [ CPU EXCEPTION ] ===================\n");
    kprintf("Vector:     %llu (%s)\n", (unsigned long long)frame->vector, name);
    kprintf("Error Code: 0x%016llx\n", (unsigned long long)frame->error_code);
    kprintf("RIP:        0x%016llx  CS:     0x%04llx\n", (unsigned long long)frame->rip, (unsigned long long)frame->cs);
    kprintf("RFLAGS:     0x%016llx  RSP:    0x%016llx  SS: 0x%04llx\n", 
            (unsigned long long)frame->rflags, (unsigned long long)frame->rsp, (unsigned long long)frame->ss);
    kprintf("RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx\n",
            (unsigned long long)frame->rax, (unsigned long long)frame->rbx, (unsigned long long)frame->rcx);
    kprintf("RDX: 0x%016llx  RSI: 0x%016llx  RDI: 0x%016llx\n",
            (unsigned long long)frame->rdx, (unsigned long long)frame->rsi, (unsigned long long)frame->rdi);
    kprintf("RBP: 0x%016llx  R8:  0x%016llx  R9:  0x%016llx\n",
            (unsigned long long)frame->rbp, (unsigned long long)frame->r8, (unsigned long long)frame->r9);
    kprintf("R10: 0x%016llx  R11: 0x%016llx  R12: 0x%016llx\n",
            (unsigned long long)frame->r10, (unsigned long long)frame->r11, (unsigned long long)frame->r12);
    kprintf("R13: 0x%016llx  R14: 0x%016llx  R15: 0x%016llx\n",
            (unsigned long long)frame->r13, (unsigned long long)frame->r14, (unsigned long long)frame->r15);

    if (frame->vector == 14) {
        kprintf("CR2 (Fault Addr): 0x%016llx\n", (unsigned long long)read_cr2());
    }

    kprintf("CR3 (Page Table): 0x%016llx\n", (unsigned long long)read_cr3());
    kprintf("System halted.\n");
    kprintf("=========================================================\n");

    halt_forever();
}

void idt_init(void) {
    void *stubs[32] = {
        &isr_stub_0, &isr_stub_1, &isr_stub_2, &isr_stub_3,
        &isr_stub_4, &isr_stub_5, &isr_stub_6, &isr_stub_7,
        &isr_stub_8, &isr_stub_9, &isr_stub_10, &isr_stub_11,
        &isr_stub_12, &isr_stub_13, &isr_stub_14, &isr_stub_15,
        &isr_stub_16, &isr_stub_17, &isr_stub_18, &isr_stub_19,
        &isr_stub_20, &isr_stub_21, &isr_stub_22, &isr_stub_23,
        &isr_stub_24, &isr_stub_25, &isr_stub_26, &isr_stub_27,
        &isr_stub_28, &isr_stub_29, &isr_stub_30, &isr_stub_31
    };

    for (int i = 0; i < 32; i++) {
        /* 0x8E = Present, Ring 0, 64-bit Interrupt Gate */
        idt_set_gate((u8)i, stubs[i], 0x8E);
    }

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base = (u64)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idtr));
}
