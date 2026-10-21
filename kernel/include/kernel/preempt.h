#ifndef ASTRA_KERNEL_PREEMPT_H
#define ASTRA_KERNEL_PREEMPT_H

#include <kernel/types.h>
#include <kernel/arch/x86_64/cpu.h>

/*
 * AstraOS Preemption Safety & Critical Section Architecture (Phase B)
 *
 * Scope: Single-CPU Kernel Thread Execution
 *
 * Preemption Contract:
 * 1. preempt_count:
 *    - Per-thread nesting counter stored in thread_t->preempt_count.
 *    - Before threading is initialized, falls back to g_boot_preempt_count.
 *    - When preempt_count > 0, the thread is in a non-schedulable critical section.
 *    - Explicit yielding (thread_yield) and blocking (sched_block_current, event_wait,
 *      thread_sleep_ticks) while holding preempt_count > 0 are strictly rejected.
 * 2. in_interrupt():
 *    - Evaluates (g_interrupt_depth > 0).
 *    - Hardware interrupt and exception nesting is a CPU execution property (per-CPU).
 *    - Scheduling is strictly prohibited in interrupt, exception, NMI, or double-fault context.
 * 3. need_resched:
 *    - Per-thread flag stored in thread_t->need_resched (with global fallback).
 *    - Signals that a reschedule request is pending.
 * 4. scheduling_permitted():
 *    - Evaluates whether scheduling is safely permitted:
 *      (in_interrupt() == false) && (preempt_count() == 0) &&
 *      ((read_rflags() & (1ULL << 9)) != 0) && (g_sched_initialized)
 * 5. preempt_enable():
 *    - Decrements preempt_count.
 *    - Checks underflow.
 *    - If preempt_count returns to 0, need_resched is set, and scheduling_permitted() is true,
 *      safely triggers cooperative rescheduling via sched_reschedule().
 *    - If interrupts are disabled (IF=0) or nesting > 0, rescheduling is safely deferred.
 * 6. Disabling preemption does NOT disable interrupts:
 *    - Code running with preempt_disable() can still be interrupted by hardware IRQs.
 *    - Mutable state shared with IRQ handlers MUST use IRQ-safe protection (irq_save / irq_restore).
 */

void preempt_disable(void);
void preempt_enable(void);
u32 preempt_count(void);

bool need_resched(void);
void set_need_resched(void);
void clear_need_resched(void);

bool in_interrupt(void);
bool in_irq(void);
bool in_exception(void);
bool scheduling_permitted(void);

void sched_reschedule(void);

#endif /* ASTRA_KERNEL_PREEMPT_H */
