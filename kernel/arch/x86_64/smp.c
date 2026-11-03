#include <kernel/smp.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/drivers/apic.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/vmm.h>
#include <kernel/sched.h>
#include <kernel/panic.h>

#define APIC_REG_ID         0x0020
#define APIC_REG_TPR        0x0080
#define APIC_REG_SVR        0x00F0
#define APIC_REG_ESR        0x0280
#define APIC_REG_LVT_TIMER  0x0320
#define APIC_REG_LVT_LINT0  0x0350
#define APIC_REG_LVT_LINT1  0x0360
#define APIC_REG_LVT_ERROR  0x0370
#define APIC_REG_TIMER_INIT 0x0380
#define APIC_REG_TIMER_DIV  0x03E0

cpu_state_t g_cpus[MAX_CPUS];
u32 g_cpu_count = 1;
u32 g_bsp_lapic_id = 0;
bool g_smp_initialized = false;
u8 g_lapic_to_cpu[256];

smp_cpu_tlb_queue_t g_smp_tlb_queues[MAX_CPUS];
volatile u64 g_tlb_gen_counter = 0;
volatile u32 g_tlb_shootdown_count = 0;

/* Phase AQ/AR Fault Injection Controls */
volatile u32 g_tlb_inject_delay_cpu = 0xFF;
volatile u32 g_tlb_inject_delay_loops = 0;
volatile u32 g_tlb_inject_missing_ack_cpu = 0xFF;
volatile bool g_tlb_fail_on_timeout = true;

void smp_tlb_fault_inject_reset(void) {
    g_tlb_inject_delay_cpu = 0xFF;
    g_tlb_inject_delay_loops = 0;
    g_tlb_inject_missing_ack_cpu = 0xFF;
    g_tlb_fail_on_timeout = true;
}

u64 smp_get_tlb_seq(void) {
    return g_tlb_gen_counter;
}

void smp_service_pending_tlb_requests(u32 my_cpu) {
    /* Phase AQ/AR Fault Injection: Drop ACK if requested */
    if (g_tlb_inject_missing_ack_cpu == my_cpu) {
        return;
    }

    /* Phase AQ/AR Fault Injection: Delay ACK if requested */
    if (g_tlb_inject_delay_cpu == my_cpu && g_tlb_inject_delay_loops > 0) {
        for (volatile u32 d = 0; d < g_tlb_inject_delay_loops; d++) {
            cpu_pause();
        }
    }

    for (u32 s = 0; s < g_cpu_count; s++) {
        if (s == my_cpu) continue;

        for (u32 idx = 0; idx < SMP_TLB_SLOTS_PER_CPU; idx++) {
            smp_tlb_slot_t *slot = &g_smp_tlb_queues[s].slots[idx];

            /* Fast pre-filter: if slot is FREE or RETIRED, or not targeted, or already acked, skip */
            tlb_slot_state_t quick_st = __atomic_load_n(&slot->state, __ATOMIC_SEQ_CST);
            if (quick_st != TLB_SLOT_ACTIVE && quick_st != TLB_SLOT_TIMED_OUT) {
                continue;
            }

            u32 quick_targets = __atomic_load_n(&slot->target_mask, __ATOMIC_SEQ_CST);
            if ((quick_targets & (1U << my_cpu)) == 0) {
                continue;
            }

            u32 quick_acks = __atomic_load_n(&slot->ack_mask, __ATOMIC_SEQ_CST);
            if ((quick_acks & (1U << my_cpu)) != 0) {
                /* Already acknowledged this specific slot request */
                continue;
            }

            /* Phase AU Step 1: Atomic Pre-Registration Handshake (Dekker-style Store-Load Barrier) */
            __atomic_fetch_or(&slot->in_flight_mask, (1U << my_cpu), __ATOMIC_SEQ_CST);

            /* Phase AU Step 2: Full Barrier State Validation under Sequential Consistency */
            tlb_slot_state_t st = __atomic_load_n(&slot->state, __ATOMIC_SEQ_CST);
            u32 targets = __atomic_load_n(&slot->target_mask, __ATOMIC_SEQ_CST);
            u32 acks = __atomic_load_n(&slot->ack_mask, __ATOMIC_SEQ_CST);
            u64 req_gen = __atomic_load_n(&slot->gen, __ATOMIC_SEQ_CST);

            if ((st != TLB_SLOT_ACTIVE && st != TLB_SLOT_TIMED_OUT) ||
                ((targets & (1U << my_cpu)) == 0) ||
                ((acks & (1U << my_cpu)) != 0)) {
                /* Slot transitioned, retired, or already acked before/during in-flight registration: abort and rollback */
                __atomic_fetch_and(&slot->in_flight_mask, ~(1U << my_cpu), __ATOMIC_SEQ_CST);
                continue;
            }

            /* Perform invalidation safely protected against sender reuse by in_flight_mask */
            phys_addr_t cur_cr3 = read_cr3() & CR3_ADDR_MASK;
            phys_addr_t req_pml4 = slot->pml4_phys;
            virt_addr_t va = slot->va;
            size_t count = slot->page_count;

            if (count == 0) {
                /* Whole address-space invalidation */
                if (req_pml4 == 0) {
                    write_cr3(read_cr3());
                } else if (cur_cr3 == req_pml4) {
                    write_cr3(vmm_get_kernel_pml4());
                }
            } else {
                /* Virtual address range invalidation */
                if (req_pml4 == 0 || cur_cr3 == req_pml4 || va >= 0xFFFF800000000000ULL) {
                    for (size_t p = 0; p < count; p++) {
                        invlpg(va + p * 4096);
                    }
                }
            }

            /* Phase AT: Double Generation Verification — ensure generation did not change */
            u64 post_gen = __atomic_load_n(&slot->gen, __ATOMIC_SEQ_CST);
            if (post_gen == req_gen) {
                /* Record completion with release fence and generation in ack_gens, then update ack_mask */
                __atomic_store_n(&slot->ack_gens[my_cpu], req_gen, __ATOMIC_SEQ_CST);
                g_cpus[my_cpu].acked_tlb_seq = req_gen;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                u32 new_acks = __atomic_fetch_or(&slot->ack_mask, (1U << my_cpu), __ATOMIC_SEQ_CST) | (1U << my_cpu);

                /* Phase AV: Sample current slot state so timeout that occurred mid-invalidation is safely detected */
                tlb_slot_state_t cur_st = __atomic_load_n(&slot->state, __ATOMIC_SEQ_CST);
                if (cur_st == TLB_SLOT_TIMED_OUT && (new_acks & targets) == targets) {
                    __atomic_store_n(&slot->state, TLB_SLOT_RETIRED, __ATOMIC_SEQ_CST);
                    __atomic_store_n(&slot->target_mask, 0, __ATOMIC_SEQ_CST);
                }
            }

            /* Release in-flight reader registration */
            __atomic_fetch_and(&slot->in_flight_mask, ~(1U << my_cpu), __ATOMIC_SEQ_CST);
        }
    }
}

void smp_handle_tlb_shootdown(void) {
    u32 my_cpu = smp_cur_cpu_id();
    g_cpus[my_cpu].tlb_ipi_count++;
    smp_service_pending_tlb_requests(my_cpu);
}

bool smp_tlb_shootdown(phys_addr_t pml4_phys, virt_addr_t va, size_t page_count) {
    if (page_count == 0) {
        write_cr3(read_cr3());
    } else {
        for (size_t i = 0; i < page_count; i++) {
            invlpg(va + i * 4096);
        }
    }

    if (!g_smp_initialized || g_cpu_count <= 1) {
        return true;
    }

    u32 cur_cpu = smp_cur_cpu_id();
    u32 target_mask = 0;
    for (u32 i = 0; i < g_cpu_count; i++) {
        if (i != cur_cpu && g_cpus[i].online) {
            target_mask |= (1U << i);
        }
    }

    if (target_mask == 0) {
        return true;
    }

    /* Acquire this CPU's dedicated request queue lock */
    u64 flags = spinlock_acquire_irqsave(&g_smp_tlb_queues[cur_cpu].lock);

    /* Allocate slot: scan all SMP_TLB_SLOTS_PER_CPU slots for a FREE or RETIRED slot */
    u32 allocated_idx = 0xFFFFFFFF;
    u32 drain_timeout = 2000000ULL;

    while (drain_timeout > 0) {
        for (u32 i = 0; i < SMP_TLB_SLOTS_PER_CPU; i++) {
            u32 cand = (g_smp_tlb_queues[cur_cpu].head + i) % SMP_TLB_SLOTS_PER_CPU;
            tlb_slot_state_t st = __atomic_load_n(&g_smp_tlb_queues[cur_cpu].slots[cand].state, __ATOMIC_SEQ_CST);
            u32 in_flight = __atomic_load_n(&g_smp_tlb_queues[cur_cpu].slots[cand].in_flight_mask, __ATOMIC_SEQ_CST);

            /* Phase AV: Dual-safety dynamic retirement of timed-out slots whose delayed ACKs have fully arrived */
            if (st == TLB_SLOT_TIMED_OUT && in_flight == 0) {
                u32 cand_targets = __atomic_load_n(&g_smp_tlb_queues[cur_cpu].slots[cand].target_mask, __ATOMIC_SEQ_CST);
                u32 cand_acks = __atomic_load_n(&g_smp_tlb_queues[cur_cpu].slots[cand].ack_mask, __ATOMIC_SEQ_CST);
                if ((cand_acks & cand_targets) == cand_targets) {
                    __atomic_store_n(&g_smp_tlb_queues[cur_cpu].slots[cand].state, TLB_SLOT_RETIRED, __ATOMIC_SEQ_CST);
                    __atomic_store_n(&g_smp_tlb_queues[cur_cpu].slots[cand].target_mask, 0, __ATOMIC_SEQ_CST);
                    st = TLB_SLOT_RETIRED;
                }
            }

            if ((st == TLB_SLOT_FREE || st == TLB_SLOT_RETIRED) && in_flight == 0) {
                allocated_idx = cand;
                break;
            }
        }
        if (allocated_idx != 0xFFFFFFFF) {
            break;
        }

        /* All slots occupied or in-flight: service pending requests to avoid deadlocks */
        smp_service_pending_tlb_requests(cur_cpu);
        cpu_pause();
        drain_timeout--;
    }

    if (allocated_idx == 0xFFFFFFFF) {
        /* Queue full! All slots remain ACTIVE, TIMED_OUT or in-flight */
        kprintf("[SMP] smp_tlb_shootdown: Queue full on CPU %u! All %u slots occupied\n",
                cur_cpu, SMP_TLB_SLOTS_PER_CPU);
        spinlock_release_irqrestore(&g_smp_tlb_queues[cur_cpu].lock, flags);
        return false;
    }

    smp_tlb_slot_t *slot = &g_smp_tlb_queues[cur_cpu].slots[allocated_idx];
    __atomic_store_n(&slot->state, TLB_SLOT_FREE, __ATOMIC_SEQ_CST);

    u64 req_gen = __atomic_add_fetch(&g_tlb_gen_counter, 1, __ATOMIC_SEQ_CST);

    __atomic_store_n(&slot->gen, req_gen, __ATOMIC_SEQ_CST);
    slot->sender_cpu = cur_cpu;
    slot->pml4_phys = pml4_phys;
    slot->va = va;
    slot->page_count = page_count;
    __atomic_store_n(&slot->ack_mask, 0, __ATOMIC_SEQ_CST);
    for (u32 c = 0; c < MAX_CPUS; c++) {
        __atomic_store_n(&slot->ack_gens[c], 0, __ATOMIC_SEQ_CST);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&slot->state, TLB_SLOT_ACTIVE, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&slot->target_mask, target_mask, __ATOMIC_SEQ_CST);
    g_smp_tlb_queues[cur_cpu].head = (allocated_idx + 1) % SMP_TLB_SLOTS_PER_CPU;

    /* Broadcast IPI 251 to all APs */
    apic_send_ipi_all_ex_self(APIC_TLB_IPI_VECTOR);

    /* Wait for all target CPUs to ACK this specific slot generation */
    u64 timeout = 5000000ULL;
    bool all_acked = false;
    while (timeout > 0) {
        smp_service_pending_tlb_requests(cur_cpu);

        u32 current_acks = __atomic_load_n(&slot->ack_mask, __ATOMIC_SEQ_CST);
        if ((current_acks & target_mask) == target_mask) {
            /* Verify generation match for each target */
            bool gen_matched = true;
            for (u32 t = 0; t < g_cpu_count; t++) {
                if ((target_mask & (1U << t)) != 0) {
                    if (__atomic_load_n(&slot->ack_gens[t], __ATOMIC_SEQ_CST) != req_gen) {
                        gen_matched = false;
                        break;
                    }
                }
            }
            if (gen_matched) {
                all_acked = true;
                break;
            }
        }
        cpu_pause();
        timeout--;
    }

    if (!all_acked) {
        if (g_tlb_fail_on_timeout) {
            panic("smp_tlb_shootdown: Timeout waiting for remote CPUs to ACK shootdown gen=%llu! target=0x%x, acks=0x%x",
                  req_gen, target_mask, slot->ack_mask);
        } else {
            kprintf("[SMP] smp_tlb_shootdown: Injected/detected ACK timeout for gen=%llu target=0x%x acks=0x%x\n",
                    req_gen, target_mask, slot->ack_mask);
            __atomic_store_n(&slot->state, TLB_SLOT_TIMED_OUT, __ATOMIC_SEQ_CST);
            /* Retain slot until delayed receiver drains it; release queue lock */
            spinlock_release_irqrestore(&g_smp_tlb_queues[cur_cpu].lock, flags);
            return false;
        }
    }

    __atomic_fetch_add(&g_tlb_shootdown_count, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&slot->state, TLB_SLOT_RETIRED, __ATOMIC_SEQ_CST);
    __atomic_store_n(&slot->target_mask, 0, __ATOMIC_SEQ_CST);
    spinlock_release_irqrestore(&g_smp_tlb_queues[cur_cpu].lock, flags);
    return true;
}



static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

u32 smp_cur_lapic_id(void) {
    if (!g_smp_initialized) return g_bsp_lapic_id;
    return smp_cur_cpu()->lapic_id;
}

cpu_state_t *smp_cur_cpu(void) {
    if (!g_smp_initialized) return &g_cpus[0];
    cpu_state_t *cpu = NULL;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(cpu));
    if (!cpu) return &g_cpus[0];
    return cpu;
}

u32 smp_cur_cpu_id(void) {
    if (!g_smp_initialized) return 0;
    return smp_cur_cpu()->cpu_id;
}

u32 smp_get_cpu_count(void) {
    return g_cpu_count;
}

bool smp_is_bsp(void) {
    return (smp_cur_cpu_id() == 0);
}

void smp_set_rsp0(u64 rsp0) {
    smp_cur_cpu()->tss.rsp0 = rsp0;
}

static void gdt_set_gate(struct gdt_entry *gdt, int index, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = ((base >> 16) & 0xFF);
    gdt[index].base_high = ((base >> 24) & 0xFF);

    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[index].access = access;
}

static void smp_setup_cpu_gdt(u32 cpu_id) {
    cpu_state_t *cpu = &g_cpus[cpu_id];

    /* 0x00: Null segment */
    gdt_set_gate(cpu->gdt, 0, 0, 0, 0, 0);

    /* 0x08: Kernel 64-bit code segment */
    gdt_set_gate(cpu->gdt, 1, 0, 0xFFFFF, 0x9A, 0xAF);

    /* 0x10: Kernel 64-bit data segment */
    gdt_set_gate(cpu->gdt, 2, 0, 0xFFFFF, 0x92, 0xCF);

    /* 0x18: User 64-bit data segment */
    gdt_set_gate(cpu->gdt, 3, 0, 0xFFFFF, 0xF2, 0xCF);

    /* 0x20: User 64-bit code segment */
    gdt_set_gate(cpu->gdt, 4, 0, 0xFFFFF, 0xFA, 0xAF);

    /* Configure TSS structure */
    kmemset(&cpu->tss, 0, sizeof(cpu->tss));
    cpu->tss.ist1 = (u64)&cpu->df_stack[sizeof(cpu->df_stack)];
    cpu->tss.iomap_base = sizeof(struct tss);

    u64 base = (u64)&cpu->tss;
    u32 limit = sizeof(struct tss) - 1;

    /* Entry 5: Low 8 bytes of 64-bit TSS descriptor */
    cpu->gdt[5].limit_low = (u16)(limit & 0xFFFF);
    cpu->gdt[5].base_low = (u16)(base & 0xFFFF);
    cpu->gdt[5].base_middle = (u8)((base >> 16) & 0xFF);
    cpu->gdt[5].access = 0x89; /* Present=1, Ring 0, 64-bit Available TSS */
    cpu->gdt[5].granularity = (u8)((limit >> 16) & 0x0F);
    cpu->gdt[5].base_high = (u8)((base >> 24) & 0xFF);

    /* Entry 6: High 8 bytes of 64-bit TSS descriptor */
    u32 base_upper = (u32)(base >> 32);
    cpu->gdt[6].limit_low = (u16)(base_upper & 0xFFFF);
    cpu->gdt[6].base_low = (u16)((base_upper >> 16) & 0xFFFF);
    cpu->gdt[6].base_middle = 0;
    cpu->gdt[6].access = 0;
    cpu->gdt[6].granularity = 0;
    cpu->gdt[6].base_high = 0;

    cpu->gdtr.limit = sizeof(cpu->gdt) - 1;
    cpu->gdtr.base = (u64)&cpu->gdt;
}

static void ap_load_gdt_and_tr(u32 cpu_id) {
    cpu_state_t *cpu = &g_cpus[cpu_id];

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
        : : "m"(cpu->gdtr) : "rax", "memory"
    );

    /* Load Task Register */
    __asm__ volatile ("ltr %0" : : "r"((u16)GDT_TSS_SEG) : "memory");
}

/*
 * C entry point for Application Processors released by Limine.
 */
void ap_entry_c(struct limine_smp_info *info) {
    u32 lapic_id = info->lapic_id;
    u32 cpu_id = g_lapic_to_cpu[lapic_id & 0xFF];

    /* 1. Ensure kernel page directory is active */
    write_cr3(vmm_get_kernel_pml4());

    /* 2. Load per-CPU GDT and Task Register */
    ap_load_gdt_and_tr(cpu_id);

    /* 2b. Set per-CPU GS_BASE for atomic self-lookup */
    g_cpus[cpu_id].self = &g_cpus[cpu_id];
    write_msr(0xC0000101, (u64)&g_cpus[cpu_id]);

    /* 3. Load IDT */
    idt_load();

    /* 4. Ensure Local APIC Global Enable in IA32_APIC_BASE MSR (0x1B) */
    u64 apic_base_msr = read_msr(0x1B);
    if (!(apic_base_msr & (1ULL << 11))) {
        apic_base_msr |= (1ULL << 11);
        write_msr(0x1B, apic_base_msr);
    }

    /* 5. Clear ESR */
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_ESR) = 0;
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_ESR) = 0;

    /* 6. Enable APIC + Spurious Vector 255 */
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_SVR) = 0x100 | APIC_SPURIOUS_VECTOR;

    /* 7. Accept all interrupt priorities */
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_TPR) = 0;

    /* 8. Mask LINT0, LINT1, Error */
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_LVT_LINT0) = 0x10000;
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_LVT_LINT1) = 0x10000;
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_LVT_ERROR) = 0x10000;

    /* 9. Configure Local APIC Timer: divide by 16, periodic mode, vector 32 */
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_TIMER_DIV) = 0x3;
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_LVT_TIMER) = (1 << 17) | APIC_TIMER_VECTOR;
    *(volatile u32 *)(APIC_VIRT_BASE + APIC_REG_TIMER_INIT) = 0x00080000;

    /* 10. Mark AP online */
    g_cpus[cpu_id].online = true;
    g_cpus[cpu_id].active = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    kprintf("[SMP] CPU %u online (Processor ID %u, LAPIC ID %u)\n",
            cpu_id, info->processor_id, lapic_id);
    kprintf("ASTRA_SMP_AP_STARTED\n");

    /* 11. Enter per-CPU scheduler loop */
    sched_ap_start(cpu_id);
}

void smp_init(void) {
    kprintf("\n[SMP] Initializing Symmetric Multiprocessing (SMP)...\n");

    for (int i = 0; i < 256; i++) {
        g_lapic_to_cpu[i] = 0;
    }
    for (int i = 0; i < MAX_CPUS; i++) {
        kmemset(&g_cpus[i], 0, sizeof(g_cpus[i]));
        spinlock_init(&g_smp_tlb_queues[i].lock);
        g_smp_tlb_queues[i].head = 0;
        for (int j = 0; j < SMP_TLB_SLOTS_PER_CPU; j++) {
            g_smp_tlb_queues[i].slots[j].state = TLB_SLOT_FREE;
            g_smp_tlb_queues[i].slots[j].gen = 0;
            g_smp_tlb_queues[i].slots[j].target_mask = 0;
            g_smp_tlb_queues[i].slots[j].ack_mask = 0;
            g_smp_tlb_queues[i].slots[j].in_flight_mask = 0;
            for (int k = 0; k < MAX_CPUS; k++) {
                g_smp_tlb_queues[i].slots[j].ack_gens[k] = 0;
            }
        }
    }

    struct limine_smp_response *resp = boot_get_smp_response();
    if (!resp || resp->cpu_count == 0) {
        kprintf("[SMP] Bootloader provided no SMP information. Defaulting to single-core BSP mode.\n");
        g_cpu_count = 1;
        g_bsp_lapic_id = smp_cur_lapic_id();
        g_lapic_to_cpu[g_bsp_lapic_id & 0xFF] = 0;

        g_cpus[0].cpu_id = 0;
        g_cpus[0].lapic_id = g_bsp_lapic_id;
        g_cpus[0].online = true;
        g_cpus[0].active = true;
        g_cpus[0].current_thread = thread_current();

        smp_setup_cpu_gdt(0);
        ap_load_gdt_and_tr(0);

        kprintf("[SMP] CPU 0 (BSP) initialized. Total CPUs: 1\n");
        kprintf("ASTRA_SMP_BSP_READY\n");
        kprintf("ASTRA_SMP_ALL_CPUS_READY\n\n");
        g_smp_initialized = true;
        return;
    }

    u32 total = (u32)resp->cpu_count;
    if (total > MAX_CPUS) total = MAX_CPUS;
    g_cpu_count = total;
    g_bsp_lapic_id = resp->bsp_lapic_id;

    kprintf("[SMP] Discovered %u CPUs from Limine bootloader (BSP LAPIC ID: %u)\n",
            g_cpu_count, g_bsp_lapic_id);

    /* 1. Initialize BSP CPU 0 */
    g_lapic_to_cpu[g_bsp_lapic_id & 0xFF] = 0;
    g_cpus[0].cpu_id = 0;
    g_cpus[0].lapic_id = g_bsp_lapic_id;
    g_cpus[0].online = true;
    g_cpus[0].active = true;
    g_cpus[0].current_thread = thread_current();

    smp_setup_cpu_gdt(0);
    ap_load_gdt_and_tr(0);

    /* Set BSP GS_BASE for atomic self-lookup */
    g_cpus[0].self = &g_cpus[0];
    write_msr(0xC0000101, (u64)&g_cpus[0]);

    kprintf("[SMP] CPU 0 (BSP) online (LAPIC ID %u)\n", g_bsp_lapic_id);
    kprintf("ASTRA_SMP_BSP_READY\n");

    if (g_cpu_count == 1) {
        kprintf("[SMP] Single CPU system; AP startup not required.\n");
        kprintf("ASTRA_SMP_ALL_CPUS_READY\n\n");
        g_smp_initialized = true;
        return;
    }

    /* 2. Map and bring up each AP */
    u32 ap_idx = 1;
    for (u64 i = 0; i < resp->cpu_count && ap_idx < MAX_CPUS; i++) {
        struct limine_smp_info *info = resp->cpus[i];
        if (!info) continue;
        if (info->lapic_id == g_bsp_lapic_id) {
            continue; /* Skip BSP */
        }

        u32 cpu_id = ap_idx++;
        g_lapic_to_cpu[info->lapic_id & 0xFF] = cpu_id;

        g_cpus[cpu_id].cpu_id = cpu_id;
        g_cpus[cpu_id].lapic_id = info->lapic_id;
        g_cpus[cpu_id].online = false;
        g_cpus[cpu_id].active = false;
        g_cpus[cpu_id].preempt_count = 0;
        g_cpus[cpu_id].need_resched = false;
        g_cpus[cpu_id].timer_ticks = 0;

        /* Prepare per-CPU GDT and TSS for this AP */
        smp_setup_cpu_gdt(cpu_id);

        /* Create idle thread for this AP */
        thread_t *idle = sched_create_idle_thread(cpu_id);
        g_cpus[cpu_id].idle_thread = idle;
        g_cpus[cpu_id].current_thread = idle;

        /* Release AP via Limine goto_address */
        kprintf("[SMP] Booting AP %u (Processor ID %u, LAPIC ID %u)...\n",
                cpu_id, info->processor_id, info->lapic_id);

        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        info->goto_address = ap_entry_c;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        /* Bounded wait for AP to come online (approx 50ms) */
        u64 timeout = 10000000ULL;
        while (!g_cpus[cpu_id].online && timeout > 0) {
            cpu_pause();
            timeout--;
        }

        if (g_cpus[cpu_id].online) {
            kprintf("[SMP] AP %u verified online.\n", cpu_id);
        } else {
            kprintf("[SMP] WARN: AP %u timed out during bring-up! Marking offline.\n", cpu_id);
        }
    }

    /* Verify how many CPUs came online */
    u32 online_count = 0;
    for (u32 i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].online) online_count++;
    }

    kprintf("[SMP] AP initialization complete. %u/%u CPUs online.\n", online_count, g_cpu_count);
    kprintf("ASTRA_SMP_ALL_CPUS_READY\n\n");
    g_smp_initialized = true;
}

