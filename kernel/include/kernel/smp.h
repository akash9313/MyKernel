#ifndef ASTRA_KERNEL_SMP_H
#define ASTRA_KERNEL_SMP_H

#include <kernel/types.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/sync.h>

#define MAX_CPUS 16
#define SMP_TLB_SLOTS_PER_CPU 4

struct thread;

typedef enum {
    TLB_SLOT_FREE = 0,
    TLB_SLOT_ACTIVE = 1,
    TLB_SLOT_TIMED_OUT = 2,
    TLB_SLOT_RETIRED = 3
} tlb_slot_state_t;

typedef struct {
    volatile u64 gen;                 /* Monotonic request generation ID */
    u32 sender_cpu;                   /* Initiator CPU */
    u32 _pad0;                        /* Alignment padding */
    phys_addr_t pml4_phys;            /* Target PML4 (or 0 for kernel/all) */
    virt_addr_t va;                   /* Target VA */
    size_t page_count;                /* Page count (0 = whole space) */
    volatile u32 target_mask;         /* Bitmask of target CPUs */
    volatile u32 ack_mask;            /* Bitmask of completed ACK CPUs */
    volatile u32 in_flight_mask;      /* Bitmask of remote CPUs currently servicing this slot */
    volatile tlb_slot_state_t state;  /* Current slot state */
    volatile u64 ack_gens[MAX_CPUS];  /* Per-target verified acknowledged generation */
} smp_tlb_slot_t;

typedef struct {
    spinlock_t lock;                  /* Serializes sender allocations on this CPU */
    u32 head;                         /* Next slot index to allocate */
    smp_tlb_slot_t slots[SMP_TLB_SLOTS_PER_CPU];
} smp_cpu_tlb_queue_t;

typedef struct cpu_state {
    struct cpu_state *self;         /* Self pointer for %gs:0 CPU-local lookup */
    u32 cpu_id;                     /* Logical CPU ID: 0 .. count - 1 */
    u32 lapic_id;                   /* Hardware Local APIC ID */
    volatile bool online;           /* True when AP has initialized */
    volatile bool active;           /* True when running scheduler */
    struct thread *current_thread;  /* Thread currently running on this core */
    struct thread *idle_thread;     /* Dedicated idle thread for this core */
    u32 preempt_count;              /* Per-CPU preemption disabled nesting depth */
    bool need_resched;              /* Per-CPU reschedule requested */
    volatile int interrupt_depth;   /* Per-CPU interrupt nesting depth */
    u64 timer_ticks;                /* Per-CPU Local APIC timer ticks */
    u64 last_switch_tick;
    volatile u32 tlb_ipi_count;     /* Per-CPU TLB shootdown IPI counter */
    volatile u64 acked_tlb_seq;     /* Monotonic sequence of last serviced TLB shootdown */
    struct gdt_entry gdt[7];        /* Per-CPU GDT */
    struct gdt_pointer gdtr;        /* Per-CPU GDTR */
    struct tss tss;                 /* Per-CPU TSS */
    u8 df_stack[4096] __attribute__((aligned(16)));   /* Per-CPU IST1 double fault stack */
    u8 ap_stack[16384] __attribute__((aligned(16)));  /* Per-CPU startup/idle stack */
} cpu_state_t;

extern cpu_state_t g_cpus[MAX_CPUS];
extern u32 g_cpu_count;
extern u32 g_bsp_lapic_id;
extern bool g_smp_initialized;
extern u8 g_lapic_to_cpu[256];
extern smp_cpu_tlb_queue_t g_smp_tlb_queues[MAX_CPUS];
extern volatile u64 g_tlb_gen_counter;

void smp_init(void);
u32 smp_cur_cpu_id(void);
u32 smp_cur_lapic_id(void);
cpu_state_t *smp_cur_cpu(void);
u32 smp_get_cpu_count(void);
bool smp_is_bsp(void);
void smp_set_rsp0(u64 rsp0);
bool smp_tlb_shootdown(phys_addr_t pml4_phys, virt_addr_t va, size_t page_count);
void smp_handle_tlb_shootdown(void);
void smp_service_pending_tlb_requests(u32 my_cpu);
u64 smp_get_tlb_seq(void);

/* Phase AQ/AR TLB shootdown fault injection controls */
extern volatile u32 g_tlb_inject_delay_cpu;
extern volatile u32 g_tlb_inject_delay_loops;
extern volatile u32 g_tlb_inject_missing_ack_cpu;
extern volatile bool g_tlb_fail_on_timeout;
void smp_tlb_fault_inject_reset(void);

#endif /* ASTRA_KERNEL_SMP_H */
