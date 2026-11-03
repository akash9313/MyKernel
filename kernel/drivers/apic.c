#include <kernel/drivers/apic.h>
#include <kernel/kprintf.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/vmm.h>
#include <kernel/panic.h>
#include <kernel/smp.h>

#define APIC_REG_ID         0x0020
#define APIC_REG_VERSION    0x0030
#define APIC_REG_TPR        0x0080
#define APIC_REG_EOI        0x00B0
#define APIC_REG_SVR        0x00F0
#define APIC_REG_ESR        0x0280
#define APIC_REG_LVT_TIMER  0x0320
#define APIC_REG_LVT_LINT0  0x0350
#define APIC_REG_LVT_LINT1  0x0360
#define APIC_REG_LVT_ERROR  0x0370
#define APIC_REG_TIMER_INIT 0x0380
#define APIC_REG_TIMER_CURR 0x0390
#define APIC_REG_TIMER_DIV  0x03E0
#define APIC_REG_ICR_LOW    0x0300
#define APIC_REG_ICR_HIGH   0x0310

static volatile u64 g_apic_ticks = 0;

static inline void apic_write(u32 reg, u32 val) {
    *(volatile u32 *)(APIC_VIRT_BASE + reg) = val;
}

static inline u32 apic_read(u32 reg) {
    return *(volatile u32 *)(APIC_VIRT_BASE + reg);
}

void apic_send_ipi(u32 lapic_id, u8 vector) {
    u64 timeout = 100000ULL;
    while ((apic_read(APIC_REG_ICR_LOW) & (1 << 12)) && timeout > 0) {
        cpu_pause();
        timeout--;
    }
    apic_write(APIC_REG_ICR_HIGH, (lapic_id & 0xFF) << 24);
    apic_write(APIC_REG_ICR_LOW, (1 << 14) | (u32)vector);
}

void apic_send_ipi_all_ex_self(u8 vector) {
    u64 timeout = 100000ULL;
    while ((apic_read(APIC_REG_ICR_LOW) & (1 << 12)) && timeout > 0) {
        cpu_pause();
        timeout--;
    }
    apic_write(APIC_REG_ICR_HIGH, 0);
    /* Destination shorthand 11b = All Excluding Self (bits 18..19), Assert (bit 14) */
    apic_write(APIC_REG_ICR_LOW, (3 << 18) | (1 << 14) | (u32)vector);
}

#include <kernel/sched.h>

void apic_eoi(void) {
    apic_write(APIC_REG_EOI, 0);
}

void apic_timer_handler(void) {
    if (g_smp_initialized) {
        u32 cpu_id = smp_cur_cpu_id();
        g_cpus[cpu_id].timer_ticks++;
        if (cpu_id == 0) {
            g_apic_ticks++;
            if (sched_is_initialized()) {
                sched_timer_tick(g_apic_ticks);
            }
        } else {
            if (sched_is_initialized() && thread_current() && sched_has_ready_threads()) {
                set_need_resched();
            }
        }
    } else {
        g_apic_ticks++;
        if (sched_is_initialized()) {
            sched_timer_tick(g_apic_ticks);
        }
    }
    apic_eoi();
}

u64 apic_get_ticks(void) {
    return g_apic_ticks;
}

void apic_init(void) {
    kprintf("[APIC] Initializing Local APIC and Timer...\n");

    /* 1. Mask Legacy 8259 PIC to prevent IRQ conflicts */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    io_wait();

    /* 2. Verify CPUID APIC feature flag */
    u32 eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1 << 9))) {
        panic("CPU does not support Local APIC!");
    }

    /* 3. Ensure APIC Global Enable in IA32_APIC_BASE MSR (0x1B) */
    u64 apic_base_msr = read_msr(0x1B);
    if (!(apic_base_msr & (1ULL << 11))) {
        apic_base_msr |= (1ULL << 11);
        write_msr(0x1B, apic_base_msr);
    }

    /* 4. Clear Error Status Register */
    apic_write(APIC_REG_ESR, 0);
    apic_write(APIC_REG_ESR, 0);

    /* 5. Set Spurious Interrupt Vector Register (bit 8 = APIC enable, vector 255) */
    apic_write(APIC_REG_SVR, 0x100 | APIC_SPURIOUS_VECTOR);

    /* 6. Accept all interrupt priorities via TPR */
    apic_write(APIC_REG_TPR, 0);

    /* 7. Configure LINT0 for ExtINT (8259 PIC pass-through), mask LINT1 and Error */
    apic_write(APIC_REG_LVT_LINT0, 0x0700);
    apic_write(APIC_REG_LVT_LINT1, 0x10000);
    apic_write(APIC_REG_LVT_ERROR, 0x10000);

    /* 8. Configure APIC Timer: divide by 16 (divide value 0x3) */
    apic_write(APIC_REG_TIMER_DIV, 0x3);

    /* Periodic mode (bit 17 = 1), unmasked (bit 16 = 0), vector 32 */
    apic_write(APIC_REG_LVT_TIMER, (1 << 17) | APIC_TIMER_VECTOR);

    /* Initial counter value for periodic ticks */
    apic_write(APIC_REG_TIMER_INIT, 0x00080000);

    u32 apic_id = apic_read(APIC_REG_ID) >> 24;
    u32 apic_ver = apic_read(APIC_REG_VERSION) & 0xFF;

    kprintf("[APIC] Local APIC initialized: ID=%u, Version=0x%02x, Vector=%u (Periodic)\n",
            apic_id, apic_ver, APIC_TIMER_VECTOR);
}
