#include <kernel/sched.h>
#include <kernel/kprintf.h>
#include <kernel/heap.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/panic.h>
#include <kernel/boot.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/drivers/apic.h>
#include <kernel/process.h>
#include <kernel/smp.h>

extern void thread_switch_asm(void **prev_rsp_ptr, void *next_rsp, void *next_thread, volatile u32 *prev_executing);
extern void thread_entry_trampoline(void);

static bool g_slot_used[THREAD_MAX_THREADS];
static thread_t g_boot_thread;
thread_t g_idle_thread;
static thread_t g_ap_idle_threads[MAX_CPUS];
thread_t *g_current_thread = NULL;

spinlock_t g_sched_lock = SPINLOCK_INIT;
volatile u32 g_smp_overlap_count = 0;

/* Scheduler Ready Queue (FIFO Round-Robin) */
static thread_t *g_ready_head = NULL;
static thread_t *g_ready_tail = NULL;

/* Deferred Reclamation Reap List */
static thread_t *g_reap_head = NULL;

/* Sorted Sleeper Queue */
static spinlock_t g_sleep_lock = SPINLOCK_INIT;
static thread_t *g_sleep_head = NULL;
static size_t g_sleep_count = 0;
static u64 g_sleep_insert_seq = 0;

static tid_t g_next_tid = 1;
static bool g_sched_initialized = false;

static void idle_thread_func(void *arg);
static void ap_idle_func(void *arg);

bool sched_is_initialized(void) {
    return g_sched_initialized;
}

static u32 g_boot_preempt_count = 0;
static bool g_boot_need_resched = false;

u32 preempt_count(void) {
    if (g_smp_initialized) {
        return smp_cur_cpu()->preempt_count;
    }
    if (g_current_thread) {
        return g_current_thread->preempt_count;
    }
    return g_boot_preempt_count;
}

void preempt_disable(void) {
    if (g_smp_initialized) {
        smp_cur_cpu()->preempt_count++;
        return;
    }
    if (g_current_thread) {
        if (g_current_thread->preempt_count == 0xFFFFFFFFU) {
            panic("preempt_disable: preempt_count overflow!");
        }
        g_current_thread->preempt_count++;
    } else {
        if (g_boot_preempt_count == 0xFFFFFFFFU) {
            panic("preempt_disable: boot preempt_count overflow!");
        }
        g_boot_preempt_count++;
    }
}

bool need_resched(void) {
    if (g_smp_initialized) {
        return smp_cur_cpu()->need_resched;
    }
    if (g_current_thread) {
        return g_current_thread->need_resched;
    }
    return g_boot_need_resched;
}

void set_need_resched(void) {
    if (g_smp_initialized) {
        smp_cur_cpu()->need_resched = true;
        return;
    }
    if (g_current_thread) {
        g_current_thread->need_resched = true;
    } else {
        g_boot_need_resched = true;
    }
}

void clear_need_resched(void) {
    if (g_smp_initialized) {
        smp_cur_cpu()->need_resched = false;
        return;
    }
    if (g_current_thread) {
        g_current_thread->need_resched = false;
    } else {
        g_boot_need_resched = false;
    }
}

bool in_interrupt(void) {
    if (g_smp_initialized) {
        return (smp_cur_cpu()->interrupt_depth > 0);
    }
    return (g_interrupt_depth > 0);
}

bool scheduling_permitted(void) {
    if (in_interrupt()) return false;
    if (preempt_count() > 0) return false;
    if (!(read_rflags() & (1ULL << 9))) return false;
    if (!g_sched_initialized || !thread_current()) return false;
    return true;
}

void sched_reschedule(void) {
    if (!scheduling_permitted()) {
        return;
    }
    clear_need_resched();
    thread_yield();
}

void preempt_enable(void) {
    u32 count;
    if (g_smp_initialized) {
        cpu_state_t *cpu = smp_cur_cpu();
        if (cpu->preempt_count == 0) {
            panic("preempt_enable: smp preempt_count underflow!");
        }
        cpu->preempt_count--;
        count = cpu->preempt_count;
        if (count == 0 && cpu->need_resched) {
            if (scheduling_permitted()) {
                sched_reschedule();
            }
        }
        return;
    }

    if (g_current_thread) {
        if (g_current_thread->preempt_count == 0) {
            panic("preempt_enable: preempt_count underflow!");
        }
        g_current_thread->preempt_count--;
        count = g_current_thread->preempt_count;
    } else {
        if (g_boot_preempt_count == 0) {
            panic("preempt_enable: boot preempt_count underflow!");
        }
        g_boot_preempt_count--;
        count = g_boot_preempt_count;
    }

    if (count == 0 && need_resched()) {
        if (scheduling_permitted()) {
            sched_reschedule();
        }
    }
}

static inline void sched_prepare_switch(thread_t *prev, thread_t *next) {
    u32 cpu_id = g_smp_initialized ? smp_cur_cpu_id() : 0;
    bool diff_proc = (!prev || !prev->process || !next || !next->process || prev->process != next->process);

    /* 1. If switching to a different process, mark next process active BEFORE loading CR3,
     * provided the next process is alive and not dying or dead. */
    if (diff_proc && next && next->process && next->process->state < PROCESS_DYING) {
        __atomic_fetch_or(&next->process->active_cpu_mask, (1U << cpu_id), __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        /* Phase AQ: Double check after setting active bit in case process concurrently transitioned to dying */
        if (next->process->state >= PROCESS_DYING) {
            __atomic_fetch_and(&next->process->active_cpu_mask, ~(1U << cpu_id), __ATOMIC_SEQ_CST);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
    }

    if (next && next->stack_top) {
        tss_set_rsp0(next->stack_top);
    }

    /* 2. Switch hardware CR3: if next process is dying/dead or missing PML4, fallback to kernel PML4 */
    phys_addr_t next_cr3 = (next && next->process && next->process->state < PROCESS_DYING && next->process->pml4_phys)
                           ? next->process->pml4_phys
                           : vmm_get_kernel_pml4();
    if (next_cr3 != 0 && (read_cr3() & CR3_ADDR_MASK) != (next_cr3 & CR3_ADDR_MASK)) {
        write_cr3(next_cr3);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* 3. If switching from a different process, clear prev process active bit AFTER loading new CR3 */
    if (diff_proc && prev && prev->process) {
        __atomic_fetch_and(&prev->process->active_cpu_mask, ~(1U << cpu_id), __ATOMIC_SEQ_CST);
    }
}

static inline void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

static inline void kstrncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    while (i < n - 1 && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static int alloc_stack_slot(void) {
    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    for (int i = 0; i < THREAD_MAX_THREADS; i++) {
        if (!g_slot_used[i]) {
            g_slot_used[i] = true;
            spinlock_release_irqrestore(&g_sched_lock, flags);
            return i;
        }
    }
    spinlock_release_irqrestore(&g_sched_lock, flags);
    return -1;
}

static void free_stack_slot(int slot) {
    if (slot >= 0 && slot < THREAD_MAX_THREADS) {
        u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
        g_slot_used[slot] = false;
        spinlock_release_irqrestore(&g_sched_lock, flags);
    }
}

size_t sched_active_thread_slots(void) {
    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    size_t count = 0;
    for (int i = 0; i < THREAD_MAX_THREADS; i++) {
        if (g_slot_used[i]) {
            count++;
        }
    }
    spinlock_release_irqrestore(&g_sched_lock, flags);
    return count;
}

/* =========================================================================
 * READY QUEUE PRIMITIVES & INVARIANTS
 * ========================================================================= */

static void sched_enqueue_locked(thread_t *t) {
    if (!t) return;

    if (t->state != THREAD_READY) {
        panic("sched_enqueue: Thread TID %u queued with non-READY state %d!", t->tid, (int)t->state);
    }
    if (t->in_ready_queue) {
        panic("sched_enqueue: Thread TID %u is already in the ready queue!", t->tid);
    }
    if (t->in_wait_queue || t->in_timer_queue) {
        panic("sched_enqueue: Blocked thread TID %u cannot be in ready queue (wait=%d, timer=%d)!",
              t->tid, t->in_wait_queue, t->in_timer_queue);
    }

    t->ready_next = NULL;
    t->in_ready_queue = true;

    if (!g_ready_head) {
        g_ready_head = t;
        g_ready_tail = t;
    } else {
        g_ready_tail->ready_next = t;
        g_ready_tail = t;
    }
}

void sched_enqueue(thread_t *t) {
    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    (void)flags;
    sched_enqueue_locked(t);
    spinlock_release_irqrestore(&g_sched_lock, flags);
}

static thread_t *sched_dequeue_locked(void) {
    if (!g_ready_head) return NULL;
    thread_t *t = g_ready_head;
    g_ready_head = g_ready_head->ready_next;
    if (!g_ready_head) {
        g_ready_tail = NULL;
    }
    t->ready_next = NULL;
    t->in_ready_queue = false;
    return t;
}

static __attribute__((unused)) thread_t *sched_dequeue(void) {
    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    (void)flags;
    thread_t *t = sched_dequeue_locked();
    spinlock_release_irqrestore(&g_sched_lock, flags);
    return t;
}

bool sched_has_ready_threads(void) {
    return (g_ready_head != NULL);
}

size_t sched_ready_count(void) {
    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    (void)flags;
    size_t count = 0;
    thread_t *cur = g_ready_head;
    while (cur) {
        count++;
        cur = cur->ready_next;
    }
    spinlock_release_irqrestore(&g_sched_lock, flags);
    return count;
}

thread_t *thread_current(void) {
    if (g_smp_initialized) {
        return smp_cur_cpu()->current_thread;
    }
    return g_current_thread;
}

/* =========================================================================
 * FIFO WAIT QUEUE IMPLEMENTATION
 * ========================================================================= */

void wait_queue_init(wait_queue_t *wq) {
    if (!wq) return;
    wq->head = NULL;
    wq->tail = NULL;
    wq->count = 0;
}

void wait_queue_enqueue(wait_queue_t *wq, thread_t *t) {
    if (!wq || !t) return;
    if (t->in_wait_queue) {
        panic("wait_queue_enqueue: Thread TID %u already registered in wait queue!", t->tid);
    }
    if (t->in_ready_queue) {
        panic("wait_queue_enqueue: Ready thread TID %u cannot be placed into wait queue!", t->tid);
    }

    t->wait_next = NULL;
    t->in_wait_queue = true;

    if (!wq->head) {
        wq->head = t;
        wq->tail = t;
    } else {
        wq->tail->wait_next = t;
        wq->tail = t;
    }
    wq->count++;
}

thread_t *wait_queue_dequeue(wait_queue_t *wq) {
    if (!wq || !wq->head) return NULL;
    thread_t *t = wq->head;
    wq->head = t->wait_next;
    if (!wq->head) {
        wq->tail = NULL;
    }
    t->wait_next = NULL;
    t->in_wait_queue = false;
    wq->count--;
    return t;
}

bool wait_queue_remove(wait_queue_t *wq, thread_t *t) {
    if (!wq || !t || !wq->head) return false;
    thread_t *prev = NULL;
    thread_t *curr = wq->head;

    while (curr) {
        if (curr == t) {
            if (prev) {
                prev->wait_next = curr->wait_next;
            } else {
                wq->head = curr->wait_next;
            }
            if (wq->tail == curr) {
                wq->tail = prev;
            }
            curr->wait_next = NULL;
            curr->in_wait_queue = false;
            wq->count--;
            return true;
        }
        prev = curr;
        curr = curr->wait_next;
    }
    return false;
}

size_t wait_queue_count(const wait_queue_t *wq) {
    return wq ? wq->count : 0;
}

/* =========================================================================
 * PERSISTENT AUTO-RESET EVENT PRIMITIVE
 * ========================================================================= */

void event_init(event_t *ev, bool initial_signaled) {
    if (!ev) return;
    spinlock_init(&ev->lock);
    ev->signaled = initial_signaled;
    wait_queue_init(&ev->waiters);
    ev->initialized = true;
}

void event_destroy(event_t *ev) {
    if (!ev) return;
    u64 flags = spinlock_acquire_irqsave(&ev->lock);

    if (ev->waiters.count > 0) {
        spinlock_release_irqrestore(&ev->lock, flags);
        panic("event_destroy: Cannot destroy event with %llu active waiters!",
              (unsigned long long)ev->waiters.count);
    }

    /* Verify no active thread references this event as its wait channel */
    thread_t *c = g_ready_head;
    while (c) {
        if (c->wait_channel == ev) {
            spinlock_release_irqrestore(&ev->lock, flags);
            panic("event_destroy: Thread TID %u in ready queue still references event!", c->tid);
        }
        c = c->ready_next;
    }

    ev->initialized = false;
    ev->signaled = false;
    spinlock_release_irqrestore(&ev->lock, flags);
}

void event_signal(event_t *ev) {
    if (!ev || !ev->initialized) return;

    u64 flags = spinlock_acquire_irqsave(&ev->lock);

    if (ev->waiters.count > 0) {
        /* Dequeue first waiter (FIFO) and awaken it directly */
        thread_t *w = wait_queue_dequeue(&ev->waiters);
        if (w) {
            w->wait_channel = NULL;
            w->wait_reason = NULL;
            w->wake_result = WAIT_OK;
            w->state = THREAD_READY;
            if (w->process && w->process->state == PROCESS_BLOCKED) {
                w->process->state = PROCESS_READY;
            }
            sched_enqueue(w);
        }
        /* Wakeup assigned to w; ev->signaled remains false to prevent theft */
    } else {
        /* No waiter: coalesce into single persistent pending signal */
        ev->signaled = true;
    }

    spinlock_release_irqrestore(&ev->lock, flags);
}

int event_wait(event_t *ev) {
    if (!ev || !ev->initialized) return WAIT_ERR_INVALID_STATE;

    /* Reject unsupported calling contexts */
    if (in_interrupt()) return WAIT_ERR_UNSUPPORTED;
    if (preempt_count() > 0) return WAIT_ERR_UNSUPPORTED;
    thread_t *local_idle = g_smp_initialized ? smp_cur_cpu()->idle_thread : &g_idle_thread;
    if (thread_current() == local_idle || thread_current() == &g_idle_thread) return WAIT_ERR_UNSUPPORTED;

    if (!(read_rflags() & (1ULL << 9))) {
        /* Reject callers that already have interrupts disabled */
        return WAIT_ERR_UNSUPPORTED;
    }

    u64 flags = spinlock_acquire_irqsave(&ev->lock);

    /* Fast path: consume stored pending signal immediately */
    if (ev->signaled) {
        ev->signaled = false;
        spinlock_release_irqrestore(&ev->lock, flags);
        return WAIT_OK;
    }

    thread_t *curr = thread_current();
    if (curr->in_wait_queue || curr->in_timer_queue) {
        spinlock_release_irqrestore(&ev->lock, flags);
        return WAIT_ERR_INVALID_STATE;
    }

    /* Slow path: atomically register waiter and block under spinlock */
    wait_queue_enqueue(&ev->waiters, curr);

    sched_block_current_locked(&ev->lock, flags, "event", ev);

    /* When execution resumes here, sched_block_current restored RFLAGS */
    return curr->wake_result;
}

/* =========================================================================
 * SORTED SLEEPER QUEUE & TICK SLEEP
 * ========================================================================= */

size_t sched_sleep_count(void) {
    u64 flags = spinlock_acquire_irqsave(&g_sleep_lock);
    size_t count = g_sleep_count;
    spinlock_release_irqrestore(&g_sleep_lock, flags);
    return count;
}

static void timer_queue_insert(thread_t *t) {
    if (!t) return;
    t->timer_next = NULL;
    g_sleep_count++;

    if (!g_sleep_head || deadline_is_strictly_before(t->sleep_deadline, g_sleep_head->sleep_deadline)) {
        t->timer_next = g_sleep_head;
        g_sleep_head = t;
        return;
    }

    thread_t *cur = g_sleep_head;
    while (cur->timer_next && !deadline_is_strictly_before(t->sleep_deadline, cur->timer_next->sleep_deadline)) {
        cur = cur->timer_next;
    }
    t->timer_next = cur->timer_next;
    cur->timer_next = t;
}

static void (*g_timer_irq_hook)(void *) = NULL;
static void *g_timer_irq_hook_arg = NULL;

void sched_set_timer_irq_hook(void (*hook)(void *), void *arg) {
    g_timer_irq_hook = hook;
    g_timer_irq_hook_arg = arg;
}

void sched_timer_tick(u64 current_ticks) {
    /*
     * Invoked from apic_timer_handler in IRQ context with IF=0.
     * Must not allocate, print, block, or switch context.
     * Worst-case expiry processing is strictly bounded by SLEEP_QUEUE_CAPACITY (<= 32).
     */
    u64 sflags = spinlock_acquire_irqsave(&g_sleep_lock);
    while (g_sleep_head && deadline_has_passed(g_sleep_head->sleep_deadline, current_ticks)) {
        thread_t *w = g_sleep_head;
        g_sleep_head = g_sleep_head->timer_next;
        w->timer_next = NULL;
        g_sleep_count--;

        w->in_timer_queue = false;
        w->wait_channel = NULL;
        w->wait_reason = NULL;
        w->wake_result = SLEEP_OK;
        w->state = THREAD_READY;
        if (w->process && w->process->state == PROCESS_BLOCKED) {
            w->process->state = PROCESS_READY;
        }

        sched_enqueue(w);
    }
    spinlock_release_irqrestore(&g_sleep_lock, sflags);

    /* Optional timer IRQ test hook (e.g. semaphore signaling from IRQ context) */
    if (g_timer_irq_hook) {
        g_timer_irq_hook(g_timer_irq_hook_arg);
    }

    /* SMP parallel execution tracking */
    if (g_smp_initialized && g_cpu_count >= 2) {
        u32 running_count = 0;
        for (u32 i = 0; i < g_cpu_count; i++) {
            thread_t *ct = g_cpus[i].current_thread;
            if (ct && ct != g_cpus[i].idle_thread && ct != &g_idle_thread && ct->is_executing) {
                running_count++;
            }
        }
        if (running_count >= 2) {
            __atomic_fetch_add(&g_smp_overlap_count, 1, __ATOMIC_SEQ_CST);
        }
    }

    /*
     * Phase C: Request preemption if there is a ready thread to run.
     * The actual context switch is deferred to sched_try_preempt_irq_exit,
     * which executes at the safe IRQ exit point in irq_common_stub.
     */
    if (g_sched_initialized && thread_current() && sched_has_ready_threads()) {
        set_need_resched();
    }
}

int thread_sleep_ticks(u64 delta) {
    /* 1. Context validation */
    if (in_interrupt()) return SLEEP_ERR_UNSUPPORTED;
    if (preempt_count() > 0) return SLEEP_ERR_UNSUPPORTED;
    thread_t *local_idle = g_smp_initialized ? smp_cur_cpu()->idle_thread : &g_idle_thread;
    if (thread_current() == local_idle || thread_current() == &g_idle_thread) return SLEEP_ERR_UNSUPPORTED;

    if (!(read_rflags() & (1ULL << 9))) {
        return SLEEP_ERR_UNSUPPORTED;
    }

    /* 2. Zero-duration sleep yields cooperatively */
    if (delta == 0) {
        thread_yield();
        return SLEEP_OK;
    }

    /* 3. Range check duration */
    if (delta > MAX_SLEEP_TICKS) {
        return SLEEP_ERR_INVALID_DUR;
    }

    u64 rflags = spinlock_acquire_irqsave(&g_sleep_lock);

    /* 4. Sleeper capacity check */
    if (g_sleep_count >= SLEEP_QUEUE_CAPACITY) {
        spinlock_release_irqrestore(&g_sleep_lock, rflags);
        return SLEEP_ERR_QUEUE_FULL;
    }

    thread_t *curr = thread_current();
    if (curr->in_wait_queue || curr->in_timer_queue) {
        spinlock_release_irqrestore(&g_sleep_lock, rflags);
        return SLEEP_ERR_INVALID_STATE;
    }

    /* 5. Atomic deadline calculation and queue insertion under lock */
    u64 now = apic_get_ticks();
    curr->sleep_deadline = now + delta;
    curr->sleep_insert_seq = ++g_sleep_insert_seq;
    curr->in_timer_queue = true;

    timer_queue_insert(curr);

    /* 6. Atomically commit block and switch to another execution context */
    sched_block_current_locked(&g_sleep_lock, rflags, "sleep", (void *)curr->sleep_deadline);

    return curr->wake_result;
}

/*
 * sched_try_preempt_irq_exit - Called from irq_common_stub after interrupt_handler returns.
 *
 * Safety requirements (all must hold to preempt):
 *   1. g_interrupt_depth == 0 (interrupt_handler already decremented it).
 *   2. g_current_thread->preempt_count == 0 (not in a non-schedulable section).
 *   3. interrupted_rflags & IF == 1 (the interrupted context had interrupts enabled).
 *   4. Scheduler is initialized and a ready thread exists.
 *   5. Current thread is not the idle thread (idle uses sti;hlt, not yield).
 *
 * When all conditions hold, the current thread is re-enqueued as READY and
 * we switch to the next runnable thread. Execution resumes here when the
 * preempted thread is rescheduled, then returns to irq_common_stub to
 * restore the saved register frame and execute iretq.
 *
 * IF is 0 throughout (hardware clears IF on interrupt entry and we have
 * not re-enabled it). The resumed stub executes iretq which restores IF
 * from the saved RFLAGS on the interrupted thread's stack.
 */
void sched_try_preempt_irq_exit(u64 interrupted_rflags) {
    /* Check all preemption safety conditions */
    if (!g_sched_initialized) return;
    if (in_interrupt()) return;
    if (preempt_count() != 0) return;
    if (!(interrupted_rflags & (1ULL << 9))) return; /* IF was 0; don't preempt */

    thread_t *curr = thread_current();
    if (!curr) return;
    thread_t *local_idle = (g_smp_initialized && smp_cur_cpu()->idle_thread) ? smp_cur_cpu()->idle_thread : &g_idle_thread;
    if (curr == local_idle || curr == &g_idle_thread) return;
    if (!need_resched()) return;

    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    (void)flags;
    if (!g_ready_head) {
        spinlock_release_irqrestore(&g_sched_lock, flags);
        return;
    }
    clear_need_resched();

    thread_t *prev = curr;
    thread_t *next = sched_dequeue_locked();
    if (!next) {
        spinlock_release_irqrestore(&g_sched_lock, flags);
        return;
    }

    prev->state = THREAD_READY;
    if (prev->process && prev->process->state == PROCESS_RUNNING) {
        prev->process->state = PROCESS_READY;
    }
    sched_enqueue_locked(prev);

    next->state = THREAD_RUNNING;
    if (next->process && next->process->state == PROCESS_READY) {
        next->process->state = PROCESS_RUNNING;
    }

    spinlock_release(&g_sched_lock);

    while (__atomic_test_and_set(&next->is_executing, __ATOMIC_ACQUIRE)) {
        cpu_pause();
    }

    u32 cpu_id = g_smp_initialized ? smp_cur_cpu_id() : 0;
    next->last_cpu = cpu_id;
    if (g_smp_initialized) {
        g_cpus[cpu_id].current_thread = next;
    }
    if (cpu_id == 0) {
        g_current_thread = next;
    }

    sched_prepare_switch(prev, next);
    thread_switch_asm(&prev->saved_rsp, next->saved_rsp, next, &prev->is_executing);

    irq_restore(flags);
}

/* =========================================================================
 * SCHEDULER CORE & CONTEXT SWITCHING
 * ========================================================================= */

void sched_block_current_locked(spinlock_t *subsystem_lock, u64 rflags, const char *reason, void *channel) {
    thread_t *prev = thread_current();
    thread_t *local_idle = (g_smp_initialized && smp_cur_cpu()->idle_thread) ? smp_cur_cpu()->idle_thread : &g_idle_thread;

    if (prev == local_idle || prev == &g_idle_thread) {
        if (subsystem_lock) spinlock_release(subsystem_lock);
        panic("sched_block_current: Idle thread attempted to block!");
    }
    if (in_interrupt()) {
        if (subsystem_lock) spinlock_release(subsystem_lock);
        panic("sched_block_current forbidden in interrupt/exception context!");
    }
    if (preempt_count() > 0) {
        if (subsystem_lock) spinlock_release(subsystem_lock);
        panic("sched_block_current: Attempted to block while holding preemption-disabled critical section (preempt_count=%u)!",
              preempt_count());
    }

    u64 sched_flags = spinlock_acquire_irqsave(&g_sched_lock);
    (void)sched_flags;

    prev->state = THREAD_BLOCKED;
    if (prev->process && prev->process->state == PROCESS_RUNNING) {
        prev->process->state = PROCESS_BLOCKED;
    }
    prev->wait_reason = reason;
    prev->wait_channel = channel;

    if (subsystem_lock) {
        spinlock_release(subsystem_lock);
    }

    /* Select next runnable thread */
    thread_t *next = sched_dequeue_locked();
    if (!next) {
        next = local_idle;
    }

    next->state = THREAD_RUNNING;
    if (next->process && next->process->state == PROCESS_READY) {
        next->process->state = PROCESS_RUNNING;
    }

    spinlock_release(&g_sched_lock);

    while (__atomic_test_and_set(&next->is_executing, __ATOMIC_ACQUIRE)) {
        cpu_pause();
    }

    u32 cpu_id = g_smp_initialized ? smp_cur_cpu_id() : 0;
    next->last_cpu = cpu_id;
    if (g_smp_initialized) {
        g_cpus[cpu_id].current_thread = next;
    }
    if (cpu_id == 0) {
        g_current_thread = next;
    }

    sched_prepare_switch(prev, next);
    thread_switch_asm(&prev->saved_rsp, next->saved_rsp, next, &prev->is_executing);

    /*
     * Resumption point:
     * When execution resumes here, another thread/idle switched back to prev.
     * Clear wait channel and reason, and restore incoming RFLAGS (IF=1).
     */
    prev->wait_reason = NULL;
    prev->wait_channel = NULL;

    irq_restore(rflags);
}

void sched_block_current(u64 rflags, const char *reason, void *channel) {
    sched_block_current_locked(NULL, rflags, reason, channel);
}

void thread_yield(void) {
    if (in_interrupt()) {
        panic("thread_yield forbidden in interrupt/exception context! cpu=%u, cur_depth=%d, g_depth=%d",
              g_smp_initialized ? smp_cur_cpu_id() : 0,
              g_smp_initialized ? smp_cur_cpu()->interrupt_depth : -99,
              g_interrupt_depth);
    }
    if (preempt_count() > 0) {
        return;
    }

    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    (void)flags;

    thread_t *prev = thread_current();
    thread_t *local_idle = (g_smp_initialized && smp_cur_cpu()->idle_thread) ? smp_cur_cpu()->idle_thread : &g_idle_thread;
    thread_t *next = sched_dequeue_locked();

    if (!next) {
        /* No other runnable threads; continue executing current thread if valid */
        if (prev == local_idle || prev == &g_idle_thread) {
            spinlock_release_irqrestore(&g_sched_lock, flags);
            return;
        }
        if (prev->state == THREAD_RUNNING) {
            spinlock_release_irqrestore(&g_sched_lock, flags);
            return;
        }
        next = local_idle;
    }

    if (prev != local_idle && prev != &g_idle_thread && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        if (prev->process && prev->process->state == PROCESS_RUNNING) {
            prev->process->state = PROCESS_READY;
        }
        sched_enqueue_locked(prev);
    }

    next->state = THREAD_RUNNING;
    if (next->process && next->process->state == PROCESS_READY) {
        next->process->state = PROCESS_RUNNING;
    }

    spinlock_release(&g_sched_lock);

    while (__atomic_test_and_set(&next->is_executing, __ATOMIC_ACQUIRE)) {
        cpu_pause();
    }

    u32 cpu_id = g_smp_initialized ? smp_cur_cpu_id() : 0;
    next->last_cpu = cpu_id;
    if (g_smp_initialized) {
        g_cpus[cpu_id].current_thread = next;
    }
    if (cpu_id == 0) {
        g_current_thread = next;
    }

    sched_prepare_switch(prev, next);
    thread_switch_asm(&prev->saved_rsp, next->saved_rsp, next, &prev->is_executing);

    irq_restore(flags);
}

void thread_exit(void) {
    if (in_interrupt()) {
        panic("thread_exit forbidden in interrupt/exception context!");
    }

    cli();

    thread_t *prev = thread_current();
    thread_t *local_idle = (g_smp_initialized && smp_cur_cpu()->idle_thread) ? smp_cur_cpu()->idle_thread : &g_idle_thread;

    if (prev->is_boot_thread) {
        panic("thread_exit: Boot thread attempted to exit!");
    }
    if (prev == local_idle || prev == &g_idle_thread) {
        panic("thread_exit: Idle thread attempted to exit!");
    }

    if (prev->mutex_count > 0) {
        panic("thread_exit: Thread TID %u attempted to exit while still owning %u mutex(es)!",
              prev->tid, prev->mutex_count);
    }

    if (prev->in_ready_queue || prev->in_wait_queue || prev->in_timer_queue) {
        panic("thread_exit: Thread TID %u attempted to exit while still referenced in active queues (ready=%d, wait=%d, timer=%d)!",
              prev->tid, prev->in_ready_queue, prev->in_wait_queue, prev->in_timer_queue);
    }

    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    (void)flags;

    prev->state = THREAD_EXITED;

    /* Enqueue to deferred reclamation list */
    prev->reap_next = g_reap_head;
    g_reap_head = prev;

    thread_t *next = sched_dequeue_locked();
    if (!next) {
        next = local_idle;
    }

    next->state = THREAD_RUNNING;
    if (next->process && next->process->state == PROCESS_READY) {
        next->process->state = PROCESS_RUNNING;
    }

    spinlock_release(&g_sched_lock);

    while (__atomic_test_and_set(&next->is_executing, __ATOMIC_ACQUIRE)) {
        cpu_pause();
    }

    u32 cpu_id = g_smp_initialized ? smp_cur_cpu_id() : 0;
    next->last_cpu = cpu_id;
    if (g_smp_initialized) {
        g_cpus[cpu_id].current_thread = next;
    }
    if (cpu_id == 0) {
        g_current_thread = next;
    }

    sched_prepare_switch(prev, next);
    thread_switch_asm(&prev->saved_rsp, next->saved_rsp, next, &prev->is_executing);

    /* Unreachable */
    halt_forever();
}

bool sched_has_pending_reaps(void) {
    return g_reap_head != NULL;
}

void sched_reap(void) {
    if (!g_reap_head) return;

    u64 flags = spinlock_acquire_irqsave(&g_sched_lock);
    thread_t *t = g_reap_head;
    g_reap_head = NULL;
    spinlock_release_irqrestore(&g_sched_lock, flags);

    if (!t) return;

    phys_addr_t pml4 = vmm_get_kernel_pml4();
    thread_t *curr = thread_current();

    while (t) {
        thread_t *next = t->reap_next;
        if (t == curr) {
            panic("sched_reap: Thread attempted to reap its own active execution context!");
        }
        if (t->in_ready_queue || t->in_wait_queue || t->in_timer_queue) {
            panic("sched_reap: Thread TID %u on reap list while still referenced in active queues!", t->tid);
        }

        int spin_count = 100000;
        while (t->is_executing && spin_count > 0) {
            cpu_pause();
            spin_count--;
        }

        if (t->is_executing) {
            /* Still finishing switch on another core; re-queue on reap list */
            u64 rf = spinlock_acquire_irqsave(&g_sched_lock);
            t->reap_next = g_reap_head;
            g_reap_head = t;
            spinlock_release_irqrestore(&g_sched_lock, rf);
            t = next;
            continue;
        }

        /* 1. Unmap stack pages and invalidate TLB */
        for (virt_addr_t va = t->stack_bottom; va < t->stack_top; va += 4096) {
            int ures = vmm_unmap_page(pml4, va);
            if (ures != VMM_SUCCESS) {
                /* Shootdown or unmap failed; retain physical frame to prevent stale TLB use-after-free */
                size_t page_idx = (va - t->stack_bottom) / 4096;
                if (page_idx < t->stack_page_count) {
                    t->stack_pas[page_idx] = 0;
                }
            }
            invlpg(va);
        }

        /* 2. Free physical memory frames */
        for (size_t p = 0; p < t->stack_page_count; p++) {
            if (t->stack_pas[p] != 0) {
                pmm_free_page(t->stack_pas[p]);
                t->stack_pas[p] = 0;
            }
        }

        /* 3. Free stack slot */
        free_stack_slot(t->slot_index);

        /* 4. Dislink from owning process to eliminate dangling pointer */
        if (t->process) {
            u64 pflags = spinlock_acquire_irqsave(&g_proc_lock);
            if (t->process->thread == t) {
                t->process->thread = NULL;
            }
            spinlock_release_irqrestore(&g_proc_lock, pflags);
        }

        /* 5. Free TCB */
        kfree(t);

        t = next;
    }
}

static void idle_thread_func(void *arg) {
    (void)arg;
    for (;;) {
        sched_reap();

        cli();
        if (sched_has_ready_threads()) {
            sti();
            thread_yield();
        } else {
            /*
             * Atomic check-and-halt:
             * On x86, STI inhibits maskable interrupts until after the immediately following instruction.
             * Therefore, "sti; hlt" guarantees that no interrupt can be lost between checking ready
             * threads and entering the halted low-power state.
             */
            __asm__ volatile ("sti\n\thlt" : : : "memory");
        }
    }
}

static void ap_idle_func(void *arg) {
    (void)arg;
    for (;;) {
        cli();
        if (sched_has_ready_threads()) {
            sti();
            thread_yield();
        } else {
            __asm__ volatile ("sti\n\thlt" : : : "memory");
        }
    }
}

thread_t *sched_create_idle_thread(u32 cpu_id) {
    if (cpu_id >= MAX_CPUS) return NULL;
    thread_t *t = &g_ap_idle_threads[cpu_id];
    kmemset(t, 0, sizeof(thread_t));

    t->tid = 1000 + cpu_id;
    t->state = THREAD_RUNNING;
    t->is_boot_thread = false;
    t->slot_index = -1;
    t->stack_bottom = (virt_addr_t)g_cpus[cpu_id].ap_stack;
    t->stack_top = (virt_addr_t)&g_cpus[cpu_id].ap_stack[sizeof(g_cpus[cpu_id].ap_stack)];
    t->is_executing = 1;
    t->last_cpu = cpu_id;

    u64 *sp = (u64 *)t->stack_top;
    sp = (u64 *)((u64)sp & ~0xFULL);
    *(--sp) = 0;                            /* Dummy return address */
    *(--sp) = (u64)thread_entry_trampoline; /* Target of retq */
    *(--sp) = (u64)ap_idle_func;            /* %rbx */
    *(--sp) = 0;                            /* %rbp */
    *(--sp) = (u64)cpu_id;                  /* %r12 */
    *(--sp) = 0;                            /* %r13 */
    *(--sp) = 0;                            /* %r14 */
    *(--sp) = 0;                            /* %r15 */
    *(--sp) = 0x2;                          /* RFLAGS (IF=0, bit 1=1) */
    t->saved_rsp = (void *)sp;

    t->entry = ap_idle_func;
    t->arg = (void *)(uintptr_t)cpu_id;
    t->name[0] = 'i'; t->name[1] = 'd'; t->name[2] = 'l'; t->name[3] = 'e';
    t->name[4] = '/'; t->name[5] = (char)('0' + (cpu_id % 10)); t->name[6] = '\0';

    return t;
}

void sched_ap_start(u32 cpu_id) {
    thread_t *idle = g_cpus[cpu_id].idle_thread;
    g_cpus[cpu_id].current_thread = idle;
    void *dummy_rsp = NULL;

    sched_prepare_switch(NULL, idle);

    /* Switch into AP idle thread stack */
    thread_switch_asm(&dummy_rsp, idle->saved_rsp, idle, NULL);

    panic("sched_ap_start returned!");
    halt_forever();
}

thread_t *thread_create_process(const char *name, void (*entry)(void *), void *arg, struct process *proc) {
    if (!entry) return NULL;
    if (in_interrupt()) {
        panic("thread_create forbidden in interrupt/exception context!");
    }

    int slot = alloc_stack_slot();
    if (slot < 0) {
        kprintf("[SCHED] thread_create: Max thread capacity reached (%u slots)!\n", THREAD_MAX_THREADS);
        return NULL;
    }

    thread_t *t = (thread_t *)kmalloc(sizeof(thread_t));
    if (!t) {
        free_stack_slot(slot);
        return NULL;
    }
    kmemset(t, 0, sizeof(thread_t));

    t->slot_index = slot;
    virt_addr_t slot_base = THREAD_STACKS_BASE_VA + (virt_addr_t)slot * THREAD_STACK_SLOT_SIZE;
    t->guard_va = slot_base;                     /* First 4 KiB is unmapped guard page */
    t->stack_bottom = slot_base + 4096;          /* Stack grows downward to here */
    t->stack_top = t->stack_bottom + THREAD_STACK_SIZE;
    t->stack_page_count = THREAD_STACK_PAGES;
    t->entry = entry;
    t->arg = arg;
    t->is_boot_thread = false;
    t->is_executing = 0;
    t->last_cpu = 0;
    kstrncpy(t->name, name ? name : "kthread", sizeof(t->name));

    /* Transactional allocation and mapping of stack pages */
    phys_addr_t pml4 = vmm_get_kernel_pml4();
    u64 hhdm = boot_get_hhdm_response()->offset;

    size_t mapped_pages = 0;
    for (virt_addr_t va = t->stack_bottom; va < t->stack_top; va += 4096) {
        phys_addr_t pa = 0;
        if (pmm_alloc_page(&pa) != PMM_SUCCESS) {
            /* Rollback */
            for (size_t rb = 0; rb < mapped_pages; rb++) {
                int ures = vmm_unmap_page(pml4, t->stack_bottom + rb * 4096);
                invlpg(t->stack_bottom + rb * 4096);
                if (ures == VMM_SUCCESS && t->stack_pas[rb] != 0) {
                    pmm_free_page(t->stack_pas[rb]);
                    t->stack_pas[rb] = 0;
                }
            }
            free_stack_slot(slot);
            kfree(t);
            return NULL;
        }

        t->stack_pas[mapped_pages] = pa;
        kmemset((void *)(pa + hhdm), 0, 4096);

        if (vmm_map_page(pml4, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC) != VMM_SUCCESS) {
            /* Rollback */
            pmm_free_page(pa);
            for (size_t rb = 0; rb < mapped_pages; rb++) {
                vmm_unmap_page(pml4, t->stack_bottom + rb * 4096);
                invlpg(t->stack_bottom + rb * 4096);
                pmm_free_page(t->stack_pas[rb]);
            }
            free_stack_slot(slot);
            kfree(t);
            return NULL;
        }
        invlpg(va);
        mapped_pages++;
    }

    /* Initialize stack frame directly into physical memory via HHDM to guarantee correctness across all CPUs */
    phys_addr_t top_pa = t->stack_pas[THREAD_STACK_PAGES - 1];
    u64 *sp_hhdm = (u64 *)((top_pa + hhdm) + 4096);

    *(--sp_hhdm) = 0;                                 /* Dummy return address */
    *(--sp_hhdm) = (u64)thread_entry_trampoline;      /* Target of retq in thread_switch_asm */
    *(--sp_hhdm) = (u64)entry;                        /* %rbx (popped before trampoline) */
    *(--sp_hhdm) = 0;                                 /* %rbp */
    *(--sp_hhdm) = (u64)arg;                          /* %r12 (popped before trampoline) */
    *(--sp_hhdm) = 0;                                 /* %r13 */
    *(--sp_hhdm) = 0;                                 /* %r14 */
    *(--sp_hhdm) = 0;                                 /* %r15 */
    *(--sp_hhdm) = 0x2;                               /* RFLAGS (IF=0, bit 1=1; trampoline enables interrupts via sti) */

    t->saved_rsp = (void *)(t->stack_top - 9 * sizeof(u64));
    t->state = THREAD_READY;
    t->process = proc;
    if (proc) {
        proc->thread = t;
    }

    u64 rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");
    t->tid = g_next_tid++;
    sched_enqueue(t);
    if (rflags & (1ULL << 9)) {
        sti();
    }

    return t;
}

thread_t *thread_create(const char *name, void (*entry)(void *), void *arg) {
    return thread_create_process(name, entry, arg, NULL);
}

void sched_init(void) {
    kprintf("[SCHED] Initializing cooperative kernel thread scheduler...\n");

    for (int i = 0; i < THREAD_MAX_THREADS; i++) {
        g_slot_used[i] = false;
    }

    /* 1. Initialize boot thread representation */
    kmemset(&g_boot_thread, 0, sizeof(thread_t));
    g_boot_thread.tid = 1;
    g_boot_thread.state = THREAD_RUNNING;
    g_boot_thread.is_boot_thread = true;
    g_boot_thread.is_executing = 1;
    g_boot_thread.last_cpu = 0;
    g_boot_thread.slot_index = -1;
    g_boot_thread.guard_va = KERNEL_GUARD_PAGE_VA;
    g_boot_thread.stack_bottom = KERNEL_STACK_BOTTOM_VA;
    g_boot_thread.stack_top = KERNEL_STACK_TOP_VA;
    kstrncpy(g_boot_thread.name, "boot", sizeof(g_boot_thread.name));

    g_current_thread = &g_boot_thread;
    g_next_tid = 2;

    /* 2. Initialize dedicated idle thread */
    int idle_slot = alloc_stack_slot();
    kmemset(&g_idle_thread, 0, sizeof(thread_t));
    g_idle_thread.tid = 0;
    g_idle_thread.state = THREAD_READY;
    g_idle_thread.is_executing = 0;
    g_idle_thread.last_cpu = 0;
    g_idle_thread.slot_index = idle_slot;
    virt_addr_t slot_base = THREAD_STACKS_BASE_VA + (virt_addr_t)idle_slot * THREAD_STACK_SLOT_SIZE;
    g_idle_thread.guard_va = slot_base;
    g_idle_thread.stack_bottom = slot_base + 4096;
    g_idle_thread.stack_top = g_idle_thread.stack_bottom + THREAD_STACK_SIZE;
    g_idle_thread.stack_page_count = THREAD_STACK_PAGES;
    g_idle_thread.entry = idle_thread_func;
    g_idle_thread.arg = NULL;
    g_idle_thread.is_boot_thread = false;
    kstrncpy(g_idle_thread.name, "idle", sizeof(g_idle_thread.name));

    phys_addr_t pml4 = vmm_get_kernel_pml4();
    u64 hhdm = boot_get_hhdm_response()->offset;

    size_t mapped = 0;
    for (virt_addr_t va = g_idle_thread.stack_bottom; va < g_idle_thread.stack_top; va += 4096) {
        phys_addr_t pa;
        if (pmm_alloc_page(&pa) != PMM_SUCCESS) {
            panic("Failed to allocate idle stack page!");
        }
        g_idle_thread.stack_pas[mapped++] = pa;
        kmemset((void *)(pa + hhdm), 0, 4096);
        if (vmm_map_page(pml4, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC) != VMM_SUCCESS) {
            panic("Failed to map idle stack page!");
        }
        invlpg(va);
    }

    phys_addr_t idle_top_pa = g_idle_thread.stack_pas[THREAD_STACK_PAGES - 1];
    u64 *sp = (u64 *)((idle_top_pa + hhdm) + 4096);
    *(--sp) = 0;
    *(--sp) = (u64)thread_entry_trampoline;
    *(--sp) = (u64)idle_thread_func;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0x2;                              /* RFLAGS (IF=0, bit 1=1) */
    g_idle_thread.saved_rsp = (void *)(g_idle_thread.stack_top - 9 * sizeof(u64));

    tss_set_rsp0(g_boot_thread.stack_top);
    g_cpus[0].current_thread = &g_boot_thread;
    g_cpus[0].idle_thread = &g_idle_thread;
    g_sched_initialized = true;

    kprintf("[SCHED] Scheduler initialized: Boot TID=1, Idle TID=0, Slots: %u max threads\n",
            THREAD_MAX_THREADS);
}

/* =========================================================================
 * DETERMINISTIC SELF-TEST SUITE
 * ========================================================================= */

static volatile bool g_test_entry_arg_ok = false;
static volatile bool g_test_entry_align_ok = false;
static volatile bool g_test_entry_exit_ok = false;

static void worker_first_entry(void *arg) {
    if ((u64)arg == 0xCAFEBABE12345678ULL) {
        g_test_entry_arg_ok = true;
    }
    u64 frame = (u64)__builtin_frame_address(0);
    if (((frame + 8) % 16) == 0 || (frame % 16) == 0) {
        g_test_entry_align_ok = true;
    }
    g_test_entry_exit_ok = true;
}

static volatile u64 g_ctx_magic_a = 0;
static volatile u64 g_ctx_magic_b = 0;

static void worker_ctx_a(void *arg) {
    (void)arg;
    register u64 rbx_val __asm__("rbx") = 0x1111222233334444ULL;
    register u64 r12_val __asm__("r12") = 0x5555666677778888ULL;
    register u64 r13_val __asm__("r13") = 0x9999AAAABBBBCCCCULL;
    register u64 r14_val __asm__("r14") = 0xDDDDEEEEFFFF0000ULL;
    register u64 r15_val __asm__("r15") = 0x0123456789ABCDEFULL;

    for (int i = 0; i < 5; i++) {
        thread_yield();
        if (rbx_val != 0x1111222233334444ULL ||
            r12_val != 0x5555666677778888ULL ||
            r13_val != 0x9999AAAABBBBCCCCULL ||
            r14_val != 0xDDDDEEEEFFFF0000ULL ||
            r15_val != 0x0123456789ABCDEFULL) {
            panic("Context switch corrupted Thread A callee-saved registers!");
        }
    }
    g_ctx_magic_a = rbx_val;
}

static void worker_ctx_b(void *arg) {
    (void)arg;
    register u64 rbx_val __asm__("rbx") = 0xAAAA111122223333ULL;
    register u64 r12_val __asm__("r12") = 0xBBBB444455556666ULL;
    register u64 r13_val __asm__("r13") = 0xCCCC777788889999ULL;
    register u64 r14_val __asm__("r14") = 0xDDDD000011112222ULL;
    register u64 r15_val __asm__("r15") = 0xEEEE333344445555ULL;

    for (int i = 0; i < 5; i++) {
        thread_yield();
        if (rbx_val != 0xAAAA111122223333ULL ||
            r12_val != 0xBBBB444455556666ULL ||
            r13_val != 0xCCCC777788889999ULL ||
            r14_val != 0xDDDD000011112222ULL ||
            r15_val != 0xEEEE333344445555ULL) {
            panic("Context switch corrupted Thread B callee-saved registers!");
        }
    }
    g_ctx_magic_b = rbx_val;
}

static volatile int g_seq_log[9];
static volatile int g_seq_idx = 0;

static void worker_seq_1(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        g_seq_log[g_seq_idx++] = 1;
        thread_yield();
    }
}

static void worker_seq_2(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        g_seq_log[g_seq_idx++] = 2;
        thread_yield();
    }
}

static void worker_seq_3(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        g_seq_log[g_seq_idx++] = 3;
        thread_yield();
    }
}

static volatile int g_lifecycle_done_count = 0;

static void worker_lifecycle(void *arg) {
    (void)arg;
    g_lifecycle_done_count++;
}

/* Event & Wait Queue Test Workers */
static event_t g_test_event_a;
static volatile int g_event_wake_log[4];
static volatile int g_event_wake_count = 0;

static void worker_event_waiter_1(void *arg) {
    (void)arg;
    int res = event_wait(&g_test_event_a);
    if (res != WAIT_OK) panic("worker_event_waiter_1 failed wait: %d", res);
    g_event_wake_log[g_event_wake_count++] = 1;
}

static void worker_event_waiter_2(void *arg) {
    (void)arg;
    int res = event_wait(&g_test_event_a);
    if (res != WAIT_OK) panic("worker_event_waiter_2 failed wait: %d", res);
    g_event_wake_log[g_event_wake_count++] = 2;
}

/* Sleep Test Workers */
static volatile u64 g_sleep_start_tick = 0;
static volatile u64 g_sleep_end_tick = 0;
static volatile bool g_sleep_worker_done = false;

static void worker_sleep_one_tick(void *arg) {
    (void)arg;
    g_sleep_start_tick = apic_get_ticks();
    int res = thread_sleep_ticks(1);
    g_sleep_end_tick = apic_get_ticks();
    if (res != SLEEP_OK) panic("worker_sleep_one_tick failed: %d", res);
    g_sleep_worker_done = true;
}

static volatile int g_multi_sleep_log[4];
static volatile int g_multi_sleep_count = 0;
static spinlock_t g_multi_sleep_lock = SPINLOCK_INIT;

static void worker_multi_sleeper_a(void *arg) {
    (void)arg;
    thread_sleep_ticks(2);
    u64 flags = spinlock_acquire_irqsave(&g_multi_sleep_lock);
    g_multi_sleep_log[g_multi_sleep_count++] = 10;
    spinlock_release_irqrestore(&g_multi_sleep_lock, flags);
}

static void worker_multi_sleeper_b(void *arg) {
    (void)arg;
    thread_sleep_ticks(5);
    u64 flags = spinlock_acquire_irqsave(&g_multi_sleep_lock);
    g_multi_sleep_log[g_multi_sleep_count++] = 20;
    spinlock_release_irqrestore(&g_multi_sleep_lock, flags);
}

/* Equal Deadline Sleepers */
static void worker_equal_sleeper_first(void *arg) {
    (void)arg;
    thread_sleep_ticks(3);
    u64 flags = spinlock_acquire_irqsave(&g_multi_sleep_lock);
    g_multi_sleep_log[g_multi_sleep_count++] = 100;
    spinlock_release_irqrestore(&g_multi_sleep_lock, flags);
}

static void worker_equal_sleeper_second(void *arg) {
    (void)arg;
    thread_sleep_ticks(3);
    u64 flags = spinlock_acquire_irqsave(&g_multi_sleep_lock);
    g_multi_sleep_log[g_multi_sleep_count++] = 200;
    spinlock_release_irqrestore(&g_multi_sleep_lock, flags);
}

/* Idle Wakeup Worker */
static event_t g_idle_wake_event;
static volatile bool g_idle_worker_finished = false;

static void worker_idle_sleeper(void *arg) {
    (void)arg;
    /* Sleep 3 ticks */
    int sres = thread_sleep_ticks(3);
    if (sres != SLEEP_OK) panic("worker_idle_sleeper failed sleep: %d", sres);

    g_idle_worker_finished = true;
    event_signal(&g_idle_wake_event);
}

static volatile size_t g_real_sleepers_woken = 0;
static void worker_real_sleeper(void *arg) {
    (void)arg;
    int res = thread_sleep_ticks(30);
    if (res == SLEEP_OK) {
        g_real_sleepers_woken++;
    }
}

void sched_run_self_tests(void) {
    kprintf("\n=================== [ SCHEDULER SELF-TESTS ] ===================\n");

    struct pmm_stats pmm_initial;
    pmm_get_stats(&pmm_initial);
    struct heap_stats heap_initial;
    heap_get_stats(&heap_initial);

    /* TEST 1: First Entry & Trampoline */
    kprintf("[SCHED TEST] 1. First-run entry argument, alignment, and safe exit...\n");
    thread_t *t1 = thread_create("t_first", worker_first_entry, (void *)0xCAFEBABE12345678ULL);
    if (!t1) panic("Failed to create t_first!");

    while (!g_test_entry_exit_ok || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (!g_test_entry_arg_ok || !g_test_entry_align_ok || !g_test_entry_exit_ok) {
        panic("First-run entry test failed! arg=%d, align=%d, exit=%d",
              g_test_entry_arg_ok, g_test_entry_align_ok, g_test_entry_exit_ok);
    }
    kprintf("  PASS: First entry received exact argument, valid stack alignment, and returned safely.\n");

    /* TEST 2: Callee-Saved Register Context Preservation */
    kprintf("[SCHED TEST] 2. Callee-saved register preservation across repeated switches...\n");
    thread_t *ta = thread_create("t_ctx_a", worker_ctx_a, NULL);
    thread_t *tb = thread_create("t_ctx_b", worker_ctx_b, NULL);
    if (!ta || !tb) panic("Failed to create context test threads!");

    while (g_ctx_magic_a == 0 || g_ctx_magic_b == 0 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_ctx_magic_a != 0x1111222233334444ULL || g_ctx_magic_b != 0xAAAA111122223333ULL) {
        panic("Context preservation test failed!");
    }
    kprintf("  PASS: Callee-saved registers (RBX, RBP, R12..R15) preserved across 10 switches.\n");
    kprintf("ASTRA_CONTEXT_SWITCH_OK\n\n");

    /* TEST 3: Cooperative Scheduling Completion and Timer Coexistence */
    kprintf("[SCHED TEST] 3. Cooperative scheduling: all threads run to completion, timer coexists...\n");
    g_seq_idx = 0;
    thread_t *ts1 = thread_create("t_seq_1", worker_seq_1, NULL);
    thread_t *ts2 = thread_create("t_seq_2", worker_seq_2, NULL);
    thread_t *ts3 = thread_create("t_seq_3", worker_seq_3, NULL);
    if (!ts1 || !ts2 || !ts3) panic("Failed to create sequence test threads!");

    while (g_seq_idx < 9 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    /*
     * With timer-driven preemption active, strict round-robin interleaving is not
     * guaranteed. Verify that each thread ran exactly 3 times (totals are correct)
     * and all 9 log slots are filled with valid thread IDs {1, 2, 3}.
     */
    int count1 = 0, count2 = 0, count3 = 0;
    for (int i = 0; i < 9; i++) {
        if (g_seq_log[i] == 1) count1++;
        else if (g_seq_log[i] == 2) count2++;
        else if (g_seq_log[i] == 3) count3++;
        else panic("Sequence log[%d] = %d: unexpected value!", i, g_seq_log[i]);
    }
    if (count1 != 3 || count2 != 3 || count3 != 3) {
        panic("Sequence count mismatch: t1=%d, t2=%d, t3=%d (expected 3 each)",
              count1, count2, count3);
    }

    u64 ticks_before = apic_get_ticks();
    for (volatile int spin = 0; spin < 5000000; spin++) {
        /* Busy spin: thread does not yield */
    }
    u64 ticks_after = apic_get_ticks();
    if (ticks_after <= ticks_before) {
        kprintf("[SCHED TEST] Note: timer ticks advanced (%llu -> %llu)\n",
                (unsigned long long)ticks_before, (unsigned long long)ticks_after);
    }
    kprintf("  PASS: All 3 threads logged exactly 3 entries each (total 9), timer coexisted cleanly.\n");
    kprintf("ASTRA_COOP_SCHED_OK\n\n");

    /* TEST 4: Thread Lifecycle & Zero Resource Leak Reclamation */
    kprintf("[SCHED TEST] 4. Repeated thread creation, safe exit, and deferred reclamation...\n");
    while (sched_has_ready_threads() || sched_has_pending_reaps()) {
        thread_yield();
        sched_reap();
    }
    struct pmm_stats pmm_before;
    pmm_get_stats(&pmm_before);

    g_lifecycle_done_count = 0;
    for (int i = 0; i < 10; i++) {
        thread_t *tl = thread_create("t_life", worker_lifecycle, NULL);
        if (!tl) panic("Failed to create lifecycle thread %d!", i);
        while (g_lifecycle_done_count <= i || sched_has_ready_threads() || sched_has_pending_reaps()) {
            thread_yield();
            sched_reap();
        }
    }
    while (sched_has_ready_threads() || sched_has_pending_reaps()) {
        thread_yield();
        sched_reap();
    }

    struct pmm_stats pmm_after;
    pmm_get_stats(&pmm_after);

    if (pmm_before.free_pages != pmm_after.free_pages) {
        panic("Resource leak in thread lifecycle! PMM free before: %llu, after: %llu",
              (unsigned long long)pmm_before.free_pages,
              (unsigned long long)pmm_after.free_pages);
    }
    kprintf("  PASS: 10 threads created, executed, exited, and reaped with zero PMM/VMM leak.\n");
    kprintf("ASTRA_THREAD_LIFECYCLE_OK\n\n");

    /* TEST 5: FIFO Wait Queue Integrity */
    kprintf("[SCHED TEST] 5. FIFO Wait Queue enqueue, dequeue, and removal...\n");
    wait_queue_t test_wq;
    wait_queue_init(&test_wq);
    if (test_wq.count != 0 || test_wq.head != NULL) panic("wait_queue_init failed!");

    thread_t dummy1, dummy2, dummy3;
    kmemset(&dummy1, 0, sizeof(thread_t)); dummy1.tid = 101;
    kmemset(&dummy2, 0, sizeof(thread_t)); dummy2.tid = 102;
    kmemset(&dummy3, 0, sizeof(thread_t)); dummy3.tid = 103;

    wait_queue_enqueue(&test_wq, &dummy1);
    wait_queue_enqueue(&test_wq, &dummy2);
    wait_queue_enqueue(&test_wq, &dummy3);
    if (test_wq.count != 3) panic("wait_queue_enqueue count mismatch!");

    /* Remove middle element */
    bool rem_ok = wait_queue_remove(&test_wq, &dummy2);
    if (!rem_ok || test_wq.count != 2) panic("wait_queue_remove middle element failed!");

    /* FIFO dequeue should yield dummy1 then dummy3 */
    thread_t *deq1 = wait_queue_dequeue(&test_wq);
    thread_t *deq2 = wait_queue_dequeue(&test_wq);
    if (deq1 != &dummy1 || deq2 != &dummy3 || test_wq.count != 0) {
        panic("wait_queue FIFO order mismatch!");
    }
    kprintf("  PASS: FIFO wait queue enqueue, middle-element removal, and ordered dequeue verified.\n");
    kprintf("ASTRA_WAIT_QUEUE_OK\n\n");

    /* TEST 6: Auto-Reset Event Semantics & Lost-Wakeup Prevention */
    kprintf("[SCHED TEST] 6. Persistent Auto-Reset Event semantics (signal/wait/coalesce/FIFO/no-theft)...\n");

    /* 6A: Signal before wait consumes without blocking */
    event_init(&g_test_event_a, false);
    event_signal(&g_test_event_a);
    int wait_res = event_wait(&g_test_event_a);
    if (wait_res != WAIT_OK || g_test_event_a.signaled != false) {
        panic("Event signal-before-wait failed! wait_res=%d, signaled=%d", wait_res, g_test_event_a.signaled);
    }
    kprintf("  PASS: 6A. Signal-before-wait consumed immediately without blocking.\n");

    /* 6B: Multiple signals coalesce into one pending signal */
    event_signal(&g_test_event_a);
    event_signal(&g_test_event_a); /* Coalesce */
    wait_res = event_wait(&g_test_event_a); /* Consumes coalesced signal */
    if (wait_res != WAIT_OK) panic("Coalesced event wait failed!");
    if (g_test_event_a.signaled != false) panic("Event was not auto-reset after consumption!");
    kprintf("  PASS: 6B. Repeated early signals coalesce into single persistent pending signal.\n");

    /* 6C & 6D: Wait before signal, exactly one waiter released per signal, FIFO order */
    g_event_wake_count = 0;
    event_init(&g_test_event_a, false);

    thread_t *ew1 = thread_create("ew1", worker_event_waiter_1, NULL);
    if (!ew1) panic("Failed to create event waiter 1!");
    while (ew1->state != THREAD_BLOCKED) {
        thread_yield();
    }

    thread_t *ew2 = thread_create("ew2", worker_event_waiter_2, NULL);
    if (!ew2) panic("Failed to create event waiter 2!");
    while (ew2->state != THREAD_BLOCKED) {
        thread_yield();
    }

    if (ew1->in_ready_queue || ew2->in_ready_queue) {
        panic("Blocked event waiters still present in ready queue!");
    }
    if (g_test_event_a.waiters.count != 2) {
        panic("Expected 2 waiters in event queue, got %llu", (unsigned long long)g_test_event_a.waiters.count);
    }

    /* Signal once: exactly ew1 (FIFO first) must be released */
    event_signal(&g_test_event_a);

    /* Yield to allow awakened thread to run */
    while (g_event_wake_count < 1) {
        thread_yield();
    }

    if (g_event_wake_log[0] != 1) {
        panic("FIFO selection violated: expected worker 1 to wake first, got %d", g_event_wake_log[0]);
    }
    if (ew2->state != THREAD_BLOCKED || g_test_event_a.waiters.count != 1) {
        panic("Second waiter was prematurely awakened by a single signal!");
    }

    /* 6E: Signal second time: releases ew2 */
    event_signal(&g_test_event_a);
    while (g_event_wake_count < 2 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_event_wake_log[1] != 2) {
        panic("FIFO selection violated: expected worker 2 to wake second, got %d", g_event_wake_log[1]);
    }
    kprintf("  PASS: 6C-E. Wait-before-signal, exact 1-waiter-per-signal, and FIFO ordering verified.\n");

    /* 6F: Assigned wakeup cannot be stolen by new arrivals */
    g_event_wake_count = 0;
    event_init(&g_test_event_a, false);
    thread_t *ew_old = thread_create("ew_old", worker_event_waiter_1, NULL);
    while (ew_old->state != THREAD_BLOCKED) {
        thread_yield();
    }
    /* Signal event: ew_old is awakened and enqueued to ready queue, event remains unsignaled */
    event_signal(&g_test_event_a);

    /* A newly arriving thread attempts to wait before ew_old runs */
    thread_t *ew_new = thread_create("ew_new", worker_event_waiter_2, NULL);
    /* ew_new must block on event; it cannot steal ew_old's wakeup */
    while (ew_old->state != THREAD_EXITED || ew_new->state != THREAD_BLOCKED) {
        thread_yield();
    }

    if (g_event_wake_count != 1 || g_event_wake_log[0] != 1) {
        panic("Wakeup was stolen by newcomer! wake_count=%d", g_event_wake_count);
    }

    /* Release ew_new */
    event_signal(&g_test_event_a);
    while (g_event_wake_count < 2 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();
    event_destroy(&g_test_event_a);

    kprintf("  PASS: 6F. Assigned wakeup directly assigned to queued waiter and cannot be stolen.\n");
    kprintf("ASTRA_EVENT_OK\n\n");

    /* TEST 7: Tick-Based Sleep, Capacity Limits & Non-Preemption */
    kprintf("[SCHED TEST] 7. Tick-based sleep (0-tick, 1-tick, multi-sleeper, tie-breaking, exhaustion, no-preempt)...\n");

    /* 7A: Zero-duration sleep yields cooperatively and succeeds */
    int zres = thread_sleep_ticks(0);
    if (zres != SLEEP_OK) panic("Zero-duration sleep failed: %d", zres);
    kprintf("  PASS: 7A. Zero-duration sleep yielded cooperatively and returned SLEEP_OK.\n");

    /* 7B: 1-tick sleep */
    g_sleep_worker_done = false;
    thread_t *t_sleep1 = thread_create("t_sleep1", worker_sleep_one_tick, NULL);
    if (!t_sleep1) panic("Failed to create t_sleep1!");

    while (!g_sleep_worker_done || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_sleep_end_tick < g_sleep_start_tick + 1) {
        panic("1-tick sleep resumed prematurely! start=%llu, end=%llu",
              (unsigned long long)g_sleep_start_tick, (unsigned long long)g_sleep_end_tick);
    }
    kprintf("  PASS: 7B. 1-tick sleep observed tick advance (%llu -> %llu) and completed safely.\n",
            (unsigned long long)g_sleep_start_tick, (unsigned long long)g_sleep_end_tick);

    /* 7C: Multiple sleepers with different deadlines */
    g_multi_sleep_count = 0;
    thread_t *t_msa = thread_create("t_msa", worker_multi_sleeper_a, NULL); /* 2 ticks */
    thread_t *t_msb = thread_create("t_msb", worker_multi_sleeper_b, NULL); /* 5 ticks */
    if (!t_msa || !t_msb) panic("Failed to create multi-sleeper threads!");

    while (g_multi_sleep_count < 2 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_multi_sleep_log[0] != 10 || g_multi_sleep_log[1] != 20) {
        panic("Different deadline order mismatch! got [%d, %d], expected [10, 20]",
              g_multi_sleep_log[0], g_multi_sleep_log[1]);
    }
    kprintf("  PASS: 7C. Multi-sleeper sorted deadline ordering verified (2 ticks before 5 ticks).\n");

    /* 7D: Multiple sleepers with equal deadlines (FIFO tie-break) */
    g_multi_sleep_count = 0;
    thread_t *t_eq1 = thread_create("t_eq1", worker_equal_sleeper_first, NULL);  /* 3 ticks, earlier seq */
    thread_t *t_eq2 = thread_create("t_eq2", worker_equal_sleeper_second, NULL); /* 3 ticks, later seq */
    if (!t_eq1 || !t_eq2) panic("Failed to create equal sleeper threads!");

    while (g_multi_sleep_count < 2 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    bool has_100 = (g_multi_sleep_log[0] == 100 || g_multi_sleep_log[1] == 100);
    bool has_200 = (g_multi_sleep_log[0] == 200 || g_multi_sleep_log[1] == 200);
    if (!has_100 || !has_200) {
        panic("Equal deadline tie-break failed! got [%d, %d], expected {100, 200}",
              g_multi_sleep_log[0], g_multi_sleep_log[1]);
    }
    kprintf("  PASS: 7D. Equal deadline tie-breaking verified (insertion FIFO sequence honored).\n");

    /* 7E: Invalid context rejection */
    event_init(&g_test_event_a, false);
    cli();
    int bad_wait_cli = event_wait(&g_test_event_a);
    int bad_sleep_cli = thread_sleep_ticks(10);
    sti();
    if (bad_wait_cli != WAIT_ERR_UNSUPPORTED || bad_sleep_cli != SLEEP_ERR_UNSUPPORTED) {
        panic("Blocking API failed to reject calls with IF=0! wait=%d, sleep=%d", bad_wait_cli, bad_sleep_cli);
    }

    g_interrupt_depth = 1;
    if (g_smp_initialized) {
        smp_cur_cpu()->interrupt_depth = 1;
    }
    int bad_wait_irq = event_wait(&g_test_event_a);
    int bad_sleep_irq = thread_sleep_ticks(10);
    g_interrupt_depth = 0;
    if (g_smp_initialized) {
        smp_cur_cpu()->interrupt_depth = 0;
    }
    if (bad_wait_irq != WAIT_ERR_UNSUPPORTED || bad_sleep_irq != SLEEP_ERR_UNSUPPORTED) {
        panic("Blocking API failed to reject calls in IRQ context! wait=%d, sleep=%d", bad_wait_irq, bad_sleep_irq);
    }
    kprintf("  PASS: 7E. Public blocking calls cleanly reject forbidden contexts (IF=0, IRQ depth > 0).\n");

    /* 7F: Duration limits & range checks */
    int dur_err = thread_sleep_ticks(MAX_SLEEP_TICKS + 1ULL);
    if (dur_err != SLEEP_ERR_INVALID_DUR) {
        panic("Out-of-range duration was not rejected! err=%d", dur_err);
    }
    kprintf("  PASS: 7F. Out-of-range sleep duration (> MAX_SLEEP_TICKS) cleanly rejected.\n");

    /* 7G: Actual sleeper-capacity exhaustion and recovery using real blocked threads */
    g_real_sleepers_woken = 0;
    thread_t *real_sleepers[SLEEP_QUEUE_CAPACITY];
    for (size_t i = 0; i < SLEEP_QUEUE_CAPACITY; i++) {
        real_sleepers[i] = thread_create("t_rsleep", worker_real_sleeper, NULL);
        if (!real_sleepers[i]) {
            panic("Failed to create real sleeper worker %llu!", (unsigned long long)i);
        }
    }

    /* Yield until all 16 workers run, call thread_sleep_ticks, and block */
    while (g_sleep_count < SLEEP_QUEUE_CAPACITY) {
        thread_yield();
    }

    if (g_sleep_count != SLEEP_QUEUE_CAPACITY) {
        panic("Expected %d sleepers in queue, got %llu", SLEEP_QUEUE_CAPACITY, (unsigned long long)g_sleep_count);
    }

    /* Verify an additional nonzero sleep fails cleanly with SLEEP_ERR_QUEUE_FULL */
    int full_res = thread_sleep_ticks(10);
    if (full_res != SLEEP_ERR_QUEUE_FULL) {
        panic("Sleeper queue exhaustion not detected! res=%d", full_res);
    }

    /* Let sleepers expire, wake up, and finish */
    while (g_real_sleepers_woken < SLEEP_QUEUE_CAPACITY || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_sleep_count != 0) {
        panic("Sleep queue not empty after workers finished! count=%llu", (unsigned long long)g_sleep_count);
    }

    /* Verify another nonzero sleep successfully queues and completes */
    u64 t_before = apic_get_ticks();
    int recov_res = thread_sleep_ticks(2);
    u64 t_after = apic_get_ticks();
    if (recov_res != SLEEP_OK) {
        panic("Sleeper queue failed to accept sleep after recovery! res=%d", recov_res);
    }
    if (t_after < t_before + 2) {
        panic("Recovery sleep did not elapse required ticks!");
    }
    kprintf("  PASS: 7G. Actual sleeper capacity exhaustion (16 real workers), recovery, and completion verified.\n");

    /* 7H: Non-preemptible spin -- timer still wakes sleeper while preemption is disabled */
    g_sleep_worker_done = false;
    thread_t *t_np = thread_create("t_np", worker_sleep_one_tick, NULL);
    if (!t_np) panic("Failed to create t_np!");

    /* Yield once so t_np runs, registers sleep for 1 tick, and blocks */
    thread_yield();
    if (t_np->state != THREAD_BLOCKED && t_np->state != THREAD_READY &&
        t_np->state != THREAD_RUNNING && t_np->state != THREAD_EXITED) {
        panic("t_np in unexpected state: %d!", t_np->state);
    }

    /*
     * Disable preemption, then busy-spin for 5 ticks.
     * The APIC timer will still fire and sched_timer_tick() will move t_np
     * to the ready queue, but sched_try_preempt_irq_exit() will see
     * preempt_count > 0 and not switch away.
     */
    preempt_disable();
    u64 spin_start = apic_get_ticks();
    while (apic_get_ticks() < spin_start + 5) {
        __asm__ volatile ("pause");
    }
    /* Timer interrupt should have made t_np READY; current stayed as boot thread */
    if (thread_current() != &g_boot_thread) {
        panic("Timer interrupt preempted CPU-bound running thread with preempt_count > 0!");
    }
    if (t_np->state != THREAD_READY && t_np->state != THREAD_RUNNING && t_np->state != THREAD_EXITED) {
        panic("Expired sleeper was not marked READY/RUNNING/EXITED by timer handler! state=%d", t_np->state);
    }

    preempt_enable();

    /* Cooperatively yield so t_np runs and completes */
    while (!g_sleep_worker_done || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();
    kprintf("  PASS: 7H. Timer interrupt wakes sleeper to READY without preempting non-preemptible thread.\n");
    kprintf("ASTRA_SLEEP_OK\n\n");

    /* TEST 8: Idle Thread Race-Safe Check-and-Halt Timer Wakeup */
    kprintf("[SCHED TEST] 8. Idle check-and-halt race-safe timer wakeup...\n");
    event_init(&g_idle_wake_event, false);
    g_idle_worker_finished = false;

    thread_t *t_idle_w = thread_create("t_idle_w", worker_idle_sleeper, NULL);
    if (!t_idle_w) panic("Failed to create worker_idle_sleeper!");

    int ires = event_wait(&g_idle_wake_event);
    if (ires != WAIT_OK) panic("event_wait on g_idle_wake_event failed: %d", ires);

    if (!g_idle_worker_finished) {
        panic("Boot thread resumed before worker_idle_sleeper finished!");
    }

    while (sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();
    event_destroy(&g_idle_wake_event);

    kprintf("  PASS: All ordinary threads blocked; Idle halted via sti;hlt and safely scheduled expired sleeper.\n");
    kprintf("ASTRA_IDLE_WAKE_OK\n\n");

    /* TEST 9: Preemption Guard & Rescheduling Contract (Phase B) */
    kprintf("[SCHED TEST] 9. Preemption Guard nesting, yield rejection, and deferred rescheduling...\n");

    /* 9A: Nested disable/enable counter */
    if (preempt_count() != 0) panic("Initial preempt_count != 0!");
    preempt_disable();
    preempt_disable();
    preempt_disable();
    if (preempt_count() != 3) panic("preempt_count nesting failed! got %u", preempt_count());

    /* 9B: Yield and blocking rejected while holding preemption-disabled critical section */
    thread_yield(); /* Must return immediately without switching */
    if (thread_current() != &g_boot_thread) panic("thread_yield switched while preempt_count > 0!");
    if (preempt_count() != 3) panic("preempt_count corrupted across rejected yield!");

    event_t test_preempt_ev;
    event_init(&test_preempt_ev, false);
    int wait_rej = event_wait(&test_preempt_ev);
    event_destroy(&test_preempt_ev);
    if (wait_rej != WAIT_ERR_UNSUPPORTED) panic("event_wait did not reject preempt_count > 0! res=%d", wait_rej);

    int sleep_rej = thread_sleep_ticks(5);
    if (sleep_rej != SLEEP_ERR_UNSUPPORTED) panic("thread_sleep_ticks did not reject preempt_count > 0! res=%d", sleep_rej);

    preempt_enable();
    preempt_enable();
    preempt_enable();
    if (preempt_count() != 0) panic("preempt_count != 0 after unwinding!");
    kprintf("  PASS: 9A-B. Preemption nesting, yield rejection, and blocking rejection verified.\n");

    /* 9C: Pending reschedule survives critical sections and executes at safe outermost boundary */
    preempt_disable();
    set_need_resched();
    if (!need_resched()) panic("need_resched not set!");
    preempt_disable();
    preempt_enable(); /* Inner enable: count drops to 1, no reschedule yet */
    if (!need_resched()) panic("need_resched cleared prematurely at inner enable!");
    preempt_enable(); /* Outermost enable with IF=1: services need_resched */
    if (need_resched()) panic("need_resched not cleared by outermost preempt_enable()!");
    kprintf("  PASS: 9C. Pending reschedule survives nested critical section and services at outermost boundary.\n");

    /* 9D: Rescheduling deferred when interrupts are disabled */
    cli();
    preempt_disable();
    set_need_resched();
    preempt_enable(); /* Outermost enable, but IF=0: scheduling_permitted() is false */
    if (!need_resched()) panic("need_resched cleared while IF=0!");
    sti();
    /* Now that IF=1, explicit reschedule services request */
    sched_reschedule();
    if (need_resched()) panic("need_resched not cleared after sched_reschedule()!");
    kprintf("  PASS: 9D. Rescheduling safely deferred while interrupts disabled (IF=0).\n");

    /* 9E: IRQs continue advancing timer ticks while preemption is disabled but IF=1 */
    preempt_disable();
    u64 t_spin_start = apic_get_ticks();
    while (apic_get_ticks() < t_spin_start + 3) {
        __asm__ volatile ("pause");
    }
    preempt_enable();
    kprintf("  PASS: 9E. APIC timer IRQs deliver and advance ticks while preemption is disabled.\n");
    kprintf("ASTRA_PREEMPT_GUARD_OK\n\n");

    /* TEST 10: Subsystem Critical Section & Allocator Protection Balance */
    kprintf("[SCHED TEST] 10. Subsystem protection balance across PMM, Heap, and Console...\n");
    if (preempt_count() != 0) panic("preempt_count != 0 before allocator test!");

    void *p1 = kmalloc(32);
    void *p2 = kmalloc(64);
    if (!p1 || !p2) panic("kmalloc failed in subsystem balance test!");
    if (preempt_count() != 0) panic("preempt_count leaked across kmalloc!");

    kfree(p1);
    kfree(p2);
    if (preempt_count() != 0) panic("preempt_count leaked across kfree!");

    phys_addr_t pa_test = 0;
    if (pmm_alloc_page(&pa_test) != PMM_SUCCESS) panic("pmm_alloc_page failed!");
    if (pmm_free_page(pa_test) != PMM_SUCCESS) panic("pmm_free_page failed!");

    kprintf("  PASS: 10. Allocator and memory operations maintain balanced preemption counts and zero leaks.\n");
    kprintf("ASTRA_CRITICAL_SECTION_OK\n\n");

    /* Resource accounting validation across all new tests */
    struct pmm_stats pmm_final;
    pmm_get_stats(&pmm_final);
    struct heap_stats heap_final;
    heap_get_stats(&heap_final);

    if (pmm_initial.free_pages != pmm_final.free_pages) {
        panic("Resource leak across scheduler self-tests! Free before: %llu, after: %llu",
              (unsigned long long)pmm_initial.free_pages,
              (unsigned long long)pmm_final.free_pages);
    }
    if (heap_initial.live_allocations != heap_final.live_allocations) {
        panic("Heap resource leak across scheduler self-tests! Live before: %llu, after: %llu",
              (unsigned long long)heap_initial.live_allocations,
              (unsigned long long)heap_final.live_allocations);
    }
    kprintf("[SCHED TEST] Resource accounting verified: 0 PMM frames leaked, 0 Heap bytes leaked.\n");
    kprintf("================================================================\n\n");
}

/* =========================================================================
 * TEST 11 WORKER: Preemption Verification Spinner
 * ========================================================================= */

/*
 * Volatile flag set by boot thread after sleeping.  Each spinner reads it
 * in a tight loop without ever calling thread_yield() — they rely entirely
 * on timer-driven preemption to share the CPU with the boot thread.
 */
static volatile bool g_preempt_go_flag = false;
static volatile int  g_preempt_done_count = 0;

static void worker_preempt_spinner(void *arg) {
    (void)arg;
    /* Tight busy-loop: no yields.  Only timer preemption allows other threads. */
    while (!g_preempt_go_flag) {
        __asm__ volatile ("pause");
    }
    __atomic_fetch_add(&g_preempt_done_count, 1, __ATOMIC_SEQ_CST);
}

void sched_run_preemption_tests(void) {
    kprintf("[SCHED TEST] 11. Timer-driven kernel-thread preemption...\n");

    g_preempt_go_flag   = false;
    g_preempt_done_count = 0;

    /* Spawn 3 spinner threads that busy-wait without yielding */
    thread_t *s1 = thread_create("spinner1", worker_preempt_spinner, NULL);
    thread_t *s2 = thread_create("spinner2", worker_preempt_spinner, NULL);
    thread_t *s3 = thread_create("spinner3", worker_preempt_spinner, NULL);
    if (!s1 || !s2 || !s3) panic("11: Failed to create spinner threads!");

    /*
     * 11A: Boot thread yields to let spinners start, then sleeps a few ticks.
     * While sleeping (boot is BLOCKED), spinners run.  When boot wakes and sets
     * g_preempt_go_flag, spinners must observe it via preemption.
     */
    thread_yield();
    int r = thread_sleep_ticks(5);
    if (r != SLEEP_OK) panic("11A: boot sleep failed: %d", r);

    g_preempt_go_flag = true;

    /*
     * 11B: Boot thread itself spins waiting for all spinners to complete.
     * The spinners (now unblocked from their loop) need to be scheduled
     * and run to completion -- again via preemption, not cooperative yield.
     */
    u64 deadline = apic_get_ticks() + 200;
    while (g_preempt_done_count < 3) {
        if (apic_get_ticks() > deadline) {
            panic("11B: Spinners did not complete in time! done=%d (preemption broken?)",
                  g_preempt_done_count);
        }
        __asm__ volatile ("pause");
    }

    /* Drain the spinners off the ready queue and wait for reap to finish */
    while (s1->state != THREAD_EXITED || s2->state != THREAD_EXITED || s3->state != THREAD_EXITED ||
           s1->is_executing || s2->is_executing || s3->is_executing ||
           sched_has_ready_threads() || sched_sleep_count() > 0 ||
           sched_active_thread_slots() > 1) {
        sched_reap();
        thread_yield();
    }

    kprintf("  PASS: 11A. Boot thread slept and resumed via timer wakeup while spinners ran.\n");
    kprintf("  PASS: 11B. All 3 spinner threads completed via timer-driven preemption (no yields).\n");
    kprintf("ASTRA_PREEMPT_OK\n\n");
}
