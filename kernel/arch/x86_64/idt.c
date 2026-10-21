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

extern void *irq_stub_32;
extern void *irq_stub_33;
extern void *irq_stub_34;
extern void *irq_stub_35;
extern void *irq_stub_36;
extern void *irq_stub_37;
extern void *irq_stub_38;
extern void *irq_stub_39;
extern void *irq_stub_40;
extern void *irq_stub_41;
extern void *irq_stub_42;
extern void *irq_stub_43;
extern void *irq_stub_44;
extern void *irq_stub_45;
extern void *irq_stub_46;
extern void *irq_stub_47;
extern void *irq_stub_255;

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

void idt_set_gate_ist(u8 vector, void *isr, u8 flags, u8 ist) {
    u64 addr = (u64)isr;
    g_idt[vector].isr_low = (u16)(addr & 0xFFFF);
    g_idt[vector].kernel_cs = 0x08;
    g_idt[vector].ist = ist & 0x07;
    g_idt[vector].attributes = flags;
    g_idt[vector].isr_mid = (u16)((addr >> 16) & 0xFFFF);
    g_idt[vector].isr_high = (u32)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved = 0;
}

void idt_set_gate(u8 vector, void *isr, u8 flags) {
    idt_set_gate_ist(vector, isr, flags, 0);
}

#include <kernel/boot.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/syscall.h>
#include <kernel/process.h>

static bool kstrstr_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && haystack[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

#include <kernel/drivers/apic.h>

volatile int g_interrupt_depth = 0;
volatile int g_irq_depth = 0;
volatile int g_exception_depth = 0;

bool in_irq(void) {
    return (g_irq_depth > 0);
}

bool in_exception(void) {
    return (g_exception_depth > 0);
}

extern void ps2_handle_keyboard_irq(void);
extern void ps2_handle_mouse_irq(void);

void interrupt_handler(struct interrupt_frame *frame) {
    g_interrupt_depth++;
    g_irq_depth++;
    if (frame->vector == 32) {
        apic_timer_handler();
    } else if (frame->vector == 33) {
        ps2_handle_keyboard_irq();
        outb(0x20, 0x20); /* Master PIC EOI */
        apic_eoi();
    } else if (frame->vector == 44) {
        ps2_handle_mouse_irq();
        outb(0xA0, 0x20); /* Slave PIC EOI */
        outb(0x20, 0x20); /* Master PIC EOI */
        apic_eoi();
    } else if (frame->vector == 255) {
        /* Spurious interrupt: no EOI required per Intel spec */
    } else {
        /* Generic IRQ: acknowledge with EOI */
        apic_eoi();
    }
    g_irq_depth--;
    g_interrupt_depth--;
}

void exception_handler(struct interrupt_frame *frame) {
    cli();
    g_interrupt_depth++;
    g_exception_depth++;

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

    if (frame->vector == 8) {
        u64 frame_addr = (u64)frame;
        kprintf("[TSS] Double Fault (#DF) caught on dedicated IST1 stack!\n");
        kprintf("[TSS] Fault RSP: 0x%016llx, IST1 Frame: 0x%016llx (IST1 Body: 0x%016llx .. 0x%016llx)\n",
                (unsigned long long)frame->rsp,
                (unsigned long long)frame_addr,
                (unsigned long long)DOUBLE_FAULT_STACK_BOTTOM_VA,
                (unsigned long long)DOUBLE_FAULT_STACK_TOP_VA);
        const char *cmdline = boot_get_cmdline();
        if (kstrstr_contains(cmdline, "test=df_fault")) {
            if (frame_addr >= DOUBLE_FAULT_STACK_BOTTOM_VA && frame_addr <= DOUBLE_FAULT_STACK_TOP_VA) {
                kprintf("\n[TEST] PASS: Double Fault cleanly handled on guarded IST1 stack!\n");
                kprintf("ASTRA_DF_TEST_OK\n\n");
                halt_forever();
            }
        }
    }

    if (frame->vector == 14) {
        u64 cr2 = read_cr2();
        u64 err = frame->error_code;
        bool p = (err & (1ULL << 0)) != 0;
        bool w = (err & (1ULL << 1)) != 0;
        bool u = (err & (1ULL << 2)) != 0;
        bool r = (err & (1ULL << 3)) != 0;
        bool i = (err & (1ULL << 4)) != 0;

        kprintf("CR2 (Fault Addr): 0x%016llx\n", (unsigned long long)cr2);
        kprintf("PF Flags:         Present=%d, Write=%d, User=%d, Rsvd=%d, Fetch=%d\n",
                p ? 1 : 0, w ? 1 : 0, u ? 1 : 0, r ? 1 : 0, i ? 1 : 0);
    }

    if ((frame->cs & 3) == 3) {
        /*
         * Exception occurred in Ring 3 User Mode.
         * Isolate the faulting user process, record fault details, and terminate it cleanly.
         */
        g_exception_depth--;
        g_interrupt_depth--;
        process_exit_from_fault(frame);
        /* Does not return */
    }

    if (frame->vector == 14) {
        u64 cr2 = read_cr2();
        u64 err = frame->error_code;
        bool p = (err & (1ULL << 0)) != 0;
        bool w = (err & (1ULL << 1)) != 0;
        bool i = (err & (1ULL << 4)) != 0;
        const char *cmdline = boot_get_cmdline();
        if (kstrstr_contains(cmdline, "test=ro_fault")) {
            if (cr2 == (u64)__rodata_start && p && w && !i) {
                kprintf("\n[TEST] PASS: CPU Vector 14 (#PF) caught for Read-Only violation at CR2=0x%016llx!\n",
                        (unsigned long long)cr2);
                kprintf("ASTRA_PERM_TEST_OK\n\n");
                halt_forever();
            }
        } else if (kstrstr_contains(cmdline, "test=nx_fault")) {
            if (cr2 == 0xffffffff80500000ULL && p && i) {
                kprintf("\n[TEST] PASS: CPU Vector 14 (#PF) caught for No-Execute (NX) violation at CR2=0x%016llx!\n",
                        (unsigned long long)cr2);
                kprintf("ASTRA_PERM_TEST_OK\n\n");
                halt_forever();
            }
        } else if (kstrstr_contains(cmdline, "test=guard_fault")) {
            if (cr2 == KERNEL_GUARD_PAGE_VA && !p) {
                kprintf("\n[TEST] PASS: CPU Vector 14 (#PF) caught for unmapped Guard Page at CR2=0x%016llx!\n",
                        (unsigned long long)cr2);
                kprintf("ASTRA_PERM_TEST_OK\n\n");
                halt_forever();
            }
        } else if (kstrstr_contains(cmdline, "test=heap_lower_guard")) {
            if (cr2 == HEAP_LOWER_GUARD_VA && !p) {
                kprintf("\n[TEST] PASS: CPU Vector 14 (#PF) caught for Heap Lower Guard Page at CR2=0x%016llx!\n",
                        (unsigned long long)cr2);
                kprintf("ASTRA_PERM_TEST_OK\n\n");
                halt_forever();
            }
        } else if (kstrstr_contains(cmdline, "test=heap_upper_guard")) {
            if (cr2 == HEAP_UPPER_GUARD_VA && !p) {
                kprintf("\n[TEST] PASS: CPU Vector 14 (#PF) caught for Heap Upper Guard Page at CR2=0x%016llx!\n",
                        (unsigned long long)cr2);
                kprintf("ASTRA_PERM_TEST_OK\n\n");
                halt_forever();
            }
        } else if (kstrstr_contains(cmdline, "test=thread_guard")) {
            if (cr2 == 0xffffffff80805000ULL && !p) {
                kprintf("\n[TEST] PASS: CPU Vector 14 (#PF) caught for unmapped Thread Stack Guard Page at CR2=0x%016llx!\n",
                        (unsigned long long)cr2);
                kprintf("ASTRA_THREAD_GUARD_OK\n\n");
                halt_forever();
            }
        }
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
        if (i == 8) {
            /* Vector 8 (Double Fault) uses IST1 stack */
            idt_set_gate_ist(8, stubs[8], 0x8E, 1);
        } else {
            idt_set_gate((u8)i, stubs[i], 0x8E);
        }
    }

    /* Register Hardware IRQs 32..47 */
    void *irq_stubs[16] = {
        &irq_stub_32, &irq_stub_33, &irq_stub_34, &irq_stub_35,
        &irq_stub_36, &irq_stub_37, &irq_stub_38, &irq_stub_39,
        &irq_stub_40, &irq_stub_41, &irq_stub_42, &irq_stub_43,
        &irq_stub_44, &irq_stub_45, &irq_stub_46, &irq_stub_47
    };
    for (int i = 0; i < 16; i++) {
        idt_set_gate((u8)(32 + i), irq_stubs[i], 0x8E);
    }

    /* Vector 255: Spurious Interrupt */
    idt_set_gate(255, &irq_stub_255, 0x8E);

    /* Vector 128: System Call Entry (DPL=3) */
    syscall_init();

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base = (u64)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idtr));
    kprintf("[IDT] 256-entry IDT loaded (Exceptions 0..31 with IST1 #DF, IRQs 32..47, Spurious 255)\n");
    kprintf("ASTRA_IDT_OK\n\n");
}
