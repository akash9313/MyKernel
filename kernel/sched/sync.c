#include <kernel/sync.h>
#include <kernel/types.h>
#include <kernel/sched.h>
#include <kernel/preempt.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/apic.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>

#ifdef HOST_TEST
#define SYNC_ENTER_CRITICAL(flags)   do { (flags) = 0; } while (0)
#define SYNC_LEAVE_CRITICAL(flags)   do { (void)(flags); } while (0)
#define SYNC_CHECK_IF()              (true)
#else
#define SYNC_ENTER_CRITICAL(flags)   do { __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory"); } while (0)
#define SYNC_LEAVE_CRITICAL(flags)   do { if ((flags) & (1ULL << 9)) sti(); } while (0)
#define SYNC_CHECK_IF()              ((read_rflags() & (1ULL << 9)) != 0)
#endif

/* =========================================================================
 * NON-RECURSIVE KERNEL MUTEX IMPLEMENTATION
 * ========================================================================= */

int mutex_init(mutex_t *m) {
    if (!m) return MUTEX_ERR_INVALID;
    m->owner = NULL;
    wait_queue_init(&m->waiters);
    m->initialized = true;
    return MUTEX_OK;
}

bool mutex_is_locked(const mutex_t *m) {
    if (!m || !m->initialized) return false;
    return (m->owner != NULL);
}

thread_t *mutex_get_owner(const mutex_t *m) {
    if (!m || !m->initialized) return NULL;
    return m->owner;
}

int mutex_lock(mutex_t *m) {
    /* 1. Pointer & Initialization check */
    if (!m || !m->initialized) {
        panic("mutex_lock: attempt to lock uninitialized mutex (%p)!", m);
    }

    /* 2. Context validation: Mutex acquisition may block; prohibited in non-blocking contexts */
    if (in_interrupt()) {
        panic("mutex_lock: forbidden in interrupt context!");
    }
    if (preempt_count() > 0) {
        panic("mutex_lock: forbidden while holding preemption-disabled critical section (preempt_count=%u)!",
              preempt_count());
    }

    if (!SYNC_CHECK_IF()) {
        panic("mutex_lock: forbidden while interrupts are disabled (IF=0)!");
    }

    thread_t *curr = thread_current();
    if (!curr) {
        panic("mutex_lock: no current active thread!");
    }

    /* 3. Recursive acquisition detection */
    if (m->owner == curr) {
        panic("mutex_lock: recursive acquisition attempted by TID %u (%s) on mutex %p!",
              curr->tid, curr->name, m);
    }

    /* 4. Atomic inspection & acquisition under local CLI */
    u64 flags;
    SYNC_ENTER_CRITICAL(flags);

    if (m->owner == NULL) {
        /* Uncontended path: immediate ownership */
        m->owner = curr;
        curr->mutex_count++;
        SYNC_LEAVE_CRITICAL(flags);
        return MUTEX_OK;
    }

    /*
     * Contended path:
     * Enqueue current thread into FIFO wait queue, transition to THREAD_BLOCKED,
     * and switch away to another runnable thread.
     */
    if (curr->in_wait_queue) {
        SYNC_LEAVE_CRITICAL(flags);
        panic("mutex_lock: thread TID %u already registered in wait queue!", curr->tid);
    }

    wait_queue_enqueue(&m->waiters, curr);

    sched_block_current(flags, "mutex", m);

    /*
     * Resumption Point:
     * When execution resumes here, ownership has ALREADY been directly handed off
     * to 'curr' by mutex_unlock().
     * Anti-barging invariant: m->owner == curr, and curr->mutex_count was incremented.
     */
    if (m->owner != curr) {
        panic("mutex_lock: resumed waiter TID %u does not own mutex %p (owner is %p)!",
              curr->tid, m, m->owner);
    }

    return MUTEX_OK;
}

int mutex_try_lock(mutex_t *m) {
    if (!m || !m->initialized) return MUTEX_ERR_INVALID;

    if (in_interrupt()) return MUTEX_ERR_CONTEXT;

    thread_t *curr = thread_current();
    if (!curr) return MUTEX_ERR_CONTEXT;

    if (m->owner == curr) {
        return MUTEX_ERR_RECURSIVE;
    }

    u64 flags;
    SYNC_ENTER_CRITICAL(flags);

    if (m->owner == NULL) {
        m->owner = curr;
        curr->mutex_count++;
        SYNC_LEAVE_CRITICAL(flags);
        return MUTEX_OK;
    }

    SYNC_LEAVE_CRITICAL(flags);
    return MUTEX_ERR_BUSY;
}

int mutex_unlock(mutex_t *m) {
    if (!m || !m->initialized) {
        panic("mutex_unlock: attempt to unlock uninitialized mutex (%p)!", m);
    }

    if (in_interrupt()) {
        panic("mutex_unlock: forbidden in interrupt context!");
    }

    thread_t *curr = thread_current();
    if (!curr) {
        panic("mutex_unlock: no current active thread!");
    }

    /* Non-owner unlock detection */
    if (m->owner != curr) {
        panic("mutex_unlock: TID %u (%s) attempted to unlock mutex %p owned by TID %u (%s)!",
              curr->tid, curr->name, m,
              m->owner ? m->owner->tid : 0,
              m->owner ? m->owner->name : "none");
    }

    u64 flags;
    SYNC_ENTER_CRITICAL(flags);

    if (curr->mutex_count == 0) {
        SYNC_LEAVE_CRITICAL(flags);
        panic("mutex_unlock: thread TID %u has mutex_count underflow!", curr->tid);
    }
    curr->mutex_count--;

    if (m->waiters.count > 0) {
        /*
         * Direct Ownership Handoff:
         * Dequeue head waiter (strict FIFO), assign ownership directly to that waiter,
         * and mark it READY in the scheduler.
         * Anti-barging guarantee: m->owner is immediately non-NULL (points to next_owner).
         * No newly arriving thread can steal the lock while next_owner awaits CPU scheduling.
         */
        thread_t *next_owner = wait_queue_dequeue(&m->waiters);
        if (!next_owner) {
            SYNC_LEAVE_CRITICAL(flags);
            panic("mutex_unlock: waiter dequeue returned NULL while count > 0!");
        }

        m->owner = next_owner;
        next_owner->mutex_count++;
        next_owner->state = THREAD_READY;
        sched_enqueue(next_owner);
        set_need_resched();
    } else {
        m->owner = NULL;
    }

    SYNC_LEAVE_CRITICAL(flags);

    return MUTEX_OK;
}

/* =========================================================================
 * BOUNDED COUNTING SEMAPHORE IMPLEMENTATION
 * ========================================================================= */

int semaphore_init(semaphore_t *s, u32 initial_count, u32 maximum_count) {
    if (!s) return SEMAPHORE_ERR_INVALID;
    if (maximum_count == 0 || initial_count > maximum_count) {
        return SEMAPHORE_ERR_INVALID;
    }

    s->count = initial_count;
    s->maximum_count = maximum_count;
    wait_queue_init(&s->waiters);
    s->initialized = true;
    return SEMAPHORE_OK;
}

u32 semaphore_get_count(const semaphore_t *s) {
    if (!s || !s->initialized) return 0;
    return s->count;
}

int semaphore_wait(semaphore_t *s) {
    if (!s || !s->initialized) {
        panic("semaphore_wait: attempt to wait on uninitialized semaphore (%p)!", s);
    }

    if (in_interrupt()) {
        panic("semaphore_wait: forbidden in interrupt context!");
    }
    if (preempt_count() > 0) {
        panic("semaphore_wait: forbidden while holding preemption-disabled critical section!");
    }

    if (!SYNC_CHECK_IF()) {
        panic("semaphore_wait: forbidden while interrupts are disabled (IF=0)!");
    }

    thread_t *curr = thread_current();
    if (!curr) {
        panic("semaphore_wait: no current active thread!");
    }

    u64 flags;
    SYNC_ENTER_CRITICAL(flags);

    if (s->count > 0) {
        s->count--;
        SYNC_LEAVE_CRITICAL(flags);
        return SEMAPHORE_OK;
    }

    /* Contended path: enqueue waiter and block */
    if (curr->in_wait_queue) {
        SYNC_LEAVE_CRITICAL(flags);
        panic("semaphore_wait: thread TID %u already registered in wait queue!", curr->tid);
    }

    curr->wake_result = SEMAPHORE_OK;
    wait_queue_enqueue(&s->waiters, curr);

    sched_block_current(flags, "semaphore", s);

    /*
     * Resumption Point:
     * The permit was granted directly to 'curr' by semaphore_signal() without
     * incrementing s->count. Anti-barging guarantee: no new contender could steal it.
     */
    return curr->wake_result;
}

int semaphore_try_wait(semaphore_t *s) {
    if (!s || !s->initialized) return SEMAPHORE_ERR_INVALID;
    if (in_interrupt()) return SEMAPHORE_ERR_CONTEXT;

    u64 flags;
    SYNC_ENTER_CRITICAL(flags);

    if (s->count > 0) {
        s->count--;
        SYNC_LEAVE_CRITICAL(flags);
        return SEMAPHORE_OK;
    }

    SYNC_LEAVE_CRITICAL(flags);
    return SEMAPHORE_ERR_BUSY;
}

int semaphore_signal(semaphore_t *s) {
    if (!s || !s->initialized) return SEMAPHORE_ERR_INVALID;

    /* Explicitly forbid signals from NMI or CPU exception handlers */
    if (in_exception()) {
        panic("semaphore_signal: forbidden in exception/NMI context!");
    }

    u64 flags;
    SYNC_ENTER_CRITICAL(flags);

    if (s->waiters.count > 0) {
        /*
         * Direct Permit Handoff:
         * Dequeue head waiter (strict FIFO), grant permit directly to it,
         * and make it READY in the scheduler.
         * s->count is NOT incremented, preventing new arrivals from stealing the permit.
         */
        thread_t *waiter = wait_queue_dequeue(&s->waiters);
        if (!waiter) {
            SYNC_LEAVE_CRITICAL(flags);
            panic("semaphore_signal: waiter dequeue returned NULL while count > 0!");
        }

        waiter->wake_result = SEMAPHORE_OK;
        waiter->state = THREAD_READY;
        sched_enqueue(waiter);
        set_need_resched();
    } else {
        if (s->count < s->maximum_count) {
            s->count++;
        } else {
            /* Maximum capacity reached: overflow rejected with state unchanged */
            SYNC_LEAVE_CRITICAL(flags);
            return SEMAPHORE_ERR_OVERFLOW;
        }
    }

    SYNC_LEAVE_CRITICAL(flags);

    return SEMAPHORE_OK;
}

#ifndef HOST_TEST

/* =========================================================================
 * SYNCHRONIZATION TEST SUITES & VERIFICATION
 * ========================================================================= */

/* Test 1 Shared State */
static mutex_t g_mutex_contended;
static volatile u64 g_mutex_shared_counter = 0;
static volatile int g_mutex_workers_done = 0;

static void worker_mutex_contender(void *arg) {
    (void)arg;
    for (int i = 0; i < 500; i++) {
        mutex_lock(&g_mutex_contended);
        u64 val = g_mutex_shared_counter;
        for (volatile int p = 0; p < 20; p++) {
            __asm__ volatile ("pause");
        }
        g_mutex_shared_counter = val + 1;
        mutex_unlock(&g_mutex_contended);
    }
    __atomic_fetch_add(&g_mutex_workers_done, 1, __ATOMIC_SEQ_CST);
}

/* Test 2 Shared State (FIFO handoff) */
static mutex_t g_mutex_fifo;
static volatile int g_fifo_log[4];
static volatile int g_fifo_log_idx = 0;

static void worker_fifo_a(void *arg) {
    (void)arg;
    mutex_lock(&g_mutex_fifo);
    g_fifo_log[g_fifo_log_idx++] = 1;
    mutex_unlock(&g_mutex_fifo);
}

static void worker_fifo_b(void *arg) {
    (void)arg;
    mutex_lock(&g_mutex_fifo);
    g_fifo_log[g_fifo_log_idx++] = 2;
    mutex_unlock(&g_mutex_fifo);
}

/* Test 3 Shared State (Semaphore FIFO) */
static semaphore_t g_sem_fifo;
static volatile int g_sem_fifo_log[4];
static volatile int g_sem_fifo_log_idx = 0;

static void worker_sem_fifo_1(void *arg) {
    (void)arg;
    semaphore_wait(&g_sem_fifo);
    g_sem_fifo_log[g_sem_fifo_log_idx++] = 10;
}

static void worker_sem_fifo_2(void *arg) {
    (void)arg;
    semaphore_wait(&g_sem_fifo);
    g_sem_fifo_log[g_sem_fifo_log_idx++] = 20;
}

/* Test 4 Shared State (Semaphore IRQ hook) */
static semaphore_t g_sem_irq;
static volatile bool g_sem_irq_hook_ran = false;
static volatile bool g_worker_irq_done = false;

static void timer_irq_signaler_hook(void *arg) {
    semaphore_t *s = (semaphore_t *)arg;
    if (!g_sem_irq_hook_ran) {
        g_sem_irq_hook_ran = true;
        int res = semaphore_signal(s);
        if (res != SEMAPHORE_OK) {
            panic("timer_irq_signaler_hook: semaphore_signal failed: %d", res);
        }
        sched_set_timer_irq_hook(NULL, NULL);
    }
}

static void worker_sem_irq_waiter(void *arg) {
    (void)arg;
    int r = semaphore_wait(&g_sem_irq);
    if (r != SEMAPHORE_OK) panic("worker_sem_irq_waiter failed: %d", r);
    g_worker_irq_done = true;
}

/* Test 5 Shared State (Bounded Buffer Stress) */
#define BUFFER_CAPACITY 8
#define TOTAL_STRESS_ITEMS 2000

static int g_bounded_buffer[BUFFER_CAPACITY];
static int g_buf_in = 0;
static int g_buf_out = 0;

static mutex_t g_buf_mutex;
static semaphore_t g_sem_empty;
static semaphore_t g_sem_full;

static volatile u64 g_total_produced_sum = 0;
static volatile u64 g_total_consumed_sum = 0;
static volatile int g_producers_done = 0;
static volatile int g_consumers_done = 0;
static volatile bool g_spinner_stop = false;

static void worker_stress_spinner(void *arg) {
    (void)arg;
    while (!g_spinner_stop) {
        __asm__ volatile ("pause");
    }
}

static void worker_stress_producer(void *arg) {
    int start_id = (int)(uintptr_t)arg;
    u64 local_sum = 0;

    for (int i = 0; i < (TOTAL_STRESS_ITEMS / 2); i++) {
        int val = start_id + i + 1;
        semaphore_wait(&g_sem_empty);
        mutex_lock(&g_buf_mutex);

        g_bounded_buffer[g_buf_in] = val;
        g_buf_in = (g_buf_in + 1) % BUFFER_CAPACITY;
        local_sum += val;

        mutex_unlock(&g_buf_mutex);
        semaphore_signal(&g_sem_full);
    }

    __atomic_fetch_add(&g_total_produced_sum, local_sum, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_producers_done, 1, __ATOMIC_SEQ_CST);
}

static void worker_stress_consumer(void *arg) {
    (void)arg;
    u64 local_sum = 0;

    for (int i = 0; i < (TOTAL_STRESS_ITEMS / 2); i++) {
        semaphore_wait(&g_sem_full);
        mutex_lock(&g_buf_mutex);

        int val = g_bounded_buffer[g_buf_out];
        g_buf_out = (g_buf_out + 1) % BUFFER_CAPACITY;
        local_sum += val;

        mutex_unlock(&g_buf_mutex);
        semaphore_signal(&g_sem_empty);
    }

    __atomic_fetch_add(&g_total_consumed_sum, local_sum, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_consumers_done, 1, __ATOMIC_SEQ_CST);
}

/* =========================================================================
 * PUBLIC SUITE ENTRY POINT: sched_run_sync_tests
 * ========================================================================= */

void sched_run_sync_tests(void) {
    kprintf("================= [ SYNCHRONIZATION SELF-TESTS ] =================\n");

    struct pmm_stats pmm_before;
    pmm_get_stats(&pmm_before);
    struct heap_stats heap_before;
    heap_get_stats(&heap_before);

    /* ---------------------------------------------------------------------
     * 1. Mutex Core, Try-Lock, and Contended Exclusion
     * --------------------------------------------------------------------- */
    kprintf("[SYNC TEST] 1. Mutex uncontended lock/unlock, try-lock, and preemption exclusion...\n");

    mutex_t m_simple;
    if (mutex_init(&m_simple) != MUTEX_OK) panic("1: mutex_init failed!");
    if (mutex_is_locked(&m_simple)) panic("1: new mutex is marked locked!");
    if (mutex_get_owner(&m_simple) != NULL) panic("1: new mutex has non-NULL owner!");

    /* Uncontended lock / unlock */
    if (mutex_lock(&m_simple) != MUTEX_OK) panic("1: uncontended lock failed!");
    if (!mutex_is_locked(&m_simple)) panic("1: locked mutex reports unlocked!");
    if (mutex_get_owner(&m_simple) != thread_current()) panic("1: owner mismatch!");

    /* Try-lock on owned mutex returns busy */
    if (mutex_try_lock(&m_simple) != MUTEX_ERR_RECURSIVE) {
        panic("1: recursive try-lock did not return MUTEX_ERR_RECURSIVE!");
    }

    if (mutex_unlock(&m_simple) != MUTEX_OK) panic("1: unlock failed!");
    if (mutex_is_locked(&m_simple)) panic("1: unlocked mutex reports locked!");

    /* Try-lock on unowned mutex succeeds */
    if (mutex_try_lock(&m_simple) != MUTEX_OK) panic("1: try-lock on free mutex failed!");
    if (!mutex_is_locked(&m_simple)) panic("1: try-locked mutex reports unlocked!");
    mutex_unlock(&m_simple);

    /* Contended mutual exclusion under timer preemption */
    mutex_init(&g_mutex_contended);
    g_mutex_shared_counter = 0;
    g_mutex_workers_done = 0;

    thread_t *c1 = thread_create("contend1", worker_mutex_contender, NULL);
    thread_t *c2 = thread_create("contend2", worker_mutex_contender, NULL);
    thread_t *c3 = thread_create("contend3", worker_mutex_contender, NULL);
    thread_t *c4 = thread_create("contend4", worker_mutex_contender, NULL);
    if (!c1 || !c2 || !c3 || !c4) panic("1: Failed to create contender threads!");

    while (g_mutex_workers_done < 4 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_mutex_shared_counter != 2000ULL) {
        panic("1: Mutex mutual exclusion failed! Counter: %llu (expected 2000)",
              (unsigned long long)g_mutex_shared_counter);
    }
    kprintf("  PASS: 1. Mutex uncontended operations, try-lock, and 4-way preemption exclusion verified.\n");
    kprintf("ASTRA_MUTEX_OK\n\n");

    /* ---------------------------------------------------------------------
     * 2. Mutex Direct Ownership Handoff & Anti-Barging
     * --------------------------------------------------------------------- */
    kprintf("[SYNC TEST] 2. Mutex FIFO direct handoff and anti-barging...\n");

    mutex_init(&g_mutex_fifo);
    g_fifo_log_idx = 0;

    /* Boot thread holds mutex */
    mutex_lock(&g_mutex_fifo);

    thread_t *fa = thread_create("fa", worker_fifo_a, NULL);
    if (!fa) panic("2: Failed to create FIFO waiter thread fa!");
    while (fa->state != THREAD_BLOCKED) {
        thread_yield();
    }

    thread_t *fb = thread_create("fb", worker_fifo_b, NULL);
    if (!fb) panic("2: Failed to create FIFO waiter thread fb!");
    while (fb->state != THREAD_BLOCKED) {
        thread_yield();
    }

    if (g_mutex_fifo.waiters.count != 2) {
        panic("2: Expected 2 waiters in queue, got %llu", (unsigned long long)g_mutex_fifo.waiters.count);
    }

    /*
     * Boot thread unlocks:
     * Directly hands off ownership to fa (FIFO head).
     * Disable preemption across the handoff assertion so an APIC timer tick
     * does not schedule fa before the direct handoff invariant is verified.
     */
    preempt_disable();
    mutex_unlock(&g_mutex_fifo);

    /*
     * Anti-barging assertion:
     * Even before fa executes on the CPU, m_owner is already fa!
     * An arriving contender (boot thread) calling try_lock MUST be rejected.
     */
    if (g_mutex_fifo.owner != fa) {
        preempt_enable();
        panic("2: Direct handoff failed! Owner is %p, expected fa (%p)", g_mutex_fifo.owner, fa);
    }
    int barge_res = mutex_try_lock(&g_mutex_fifo);
    if (barge_res != MUTEX_ERR_BUSY) {
        preempt_enable();
        panic("2: Anti-barging violation! New contender acquired handed-off mutex: %d", barge_res);
    }
    preempt_enable();

    /* Cooperatively yield to let fa and fb execute */
    while (fa->state != THREAD_EXITED || fb->state != THREAD_EXITED || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_fifo_log_idx != 2 || g_fifo_log[0] != 1 || g_fifo_log[1] != 2) {
        panic("2: FIFO ordering violation! [%d, %d]", g_fifo_log[0], g_fifo_log[1]);
    }
    if (g_mutex_fifo.owner != NULL) {
        panic("2: Mutex not freed after waiters completed!");
    }
    kprintf("  PASS: 2. Direct ownership handoff, FIFO ordering, and anti-barging verified.\n");
    kprintf("ASTRA_MUTEX_HANDOFF_OK\n\n");

    /* ---------------------------------------------------------------------
     * 3. Counting Semaphore Accounting, Bounds, & Anti-Barging
     * --------------------------------------------------------------------- */
    kprintf("[SYNC TEST] 3. Counting Semaphore permit accounting, overflow, and FIFO handoff...\n");

    semaphore_t sem_test;
    /* Boundary check: maximum_count == 0 must fail */
    if (semaphore_init(&sem_test, 0, 0) != SEMAPHORE_ERR_INVALID) {
        panic("3: semaphore_init with max=0 did not return error!");
    }
    /* Boundary check: initial > maximum must fail */
    if (semaphore_init(&sem_test, 5, 2) != SEMAPHORE_ERR_INVALID) {
        panic("3: semaphore_init with initial > max did not return error!");
    }

    /* Valid initialization */
    if (semaphore_init(&sem_test, 2, 4) != SEMAPHORE_OK) {
        panic("3: semaphore_init failed!");
    }
    if (semaphore_get_count(&sem_test) != 2) panic("3: initial count mismatch!");

    /* Try-wait permit consumption */
    if (semaphore_try_wait(&sem_test) != SEMAPHORE_OK) panic("3: try-wait 1 failed!");
    if (semaphore_try_wait(&sem_test) != SEMAPHORE_OK) panic("3: try-wait 2 failed!");
    if (semaphore_try_wait(&sem_test) != SEMAPHORE_ERR_BUSY) panic("3: try-wait on 0 did not return busy!");

    /* Signal up to capacity */
    if (semaphore_signal(&sem_test) != SEMAPHORE_OK) panic("3: signal 1 failed!");
    if (semaphore_signal(&sem_test) != SEMAPHORE_OK) panic("3: signal 2 failed!");
    if (semaphore_signal(&sem_test) != SEMAPHORE_OK) panic("3: signal 3 failed!");
    if (semaphore_signal(&sem_test) != SEMAPHORE_OK) panic("3: signal 4 failed!");
    if (semaphore_get_count(&sem_test) != 4) panic("3: count != 4 after 4 signals!");

    /* Overflow rejection: state must remain unchanged at 4 */
    int of_res = semaphore_signal(&sem_test);
    if (of_res != SEMAPHORE_ERR_OVERFLOW) {
        panic("3: semaphore_signal overflow did not return SEMAPHORE_ERR_OVERFLOW: %d", of_res);
    }
    if (semaphore_get_count(&sem_test) != 4) {
        panic("3: semaphore count corrupted across overflow!");
    }

    /* Semaphore FIFO & Anti-Barging */
    semaphore_init(&g_sem_fifo, 0, 5);
    g_sem_fifo_log_idx = 0;

    thread_t *s1 = thread_create("sem_w1", worker_sem_fifo_1, NULL);
    thread_t *s2 = thread_create("sem_w2", worker_sem_fifo_2, NULL);
    if (!s1 || !s2) panic("3: Failed to create semaphore waiter threads!");

    while (s1->state != THREAD_BLOCKED || s2->state != THREAD_BLOCKED) {
        thread_yield();
    }
    if (g_sem_fifo.waiters.count != 2) panic("3: Waiters count mismatch!");

    /* Signal once: directly transfers permit to s1 without incrementing count */
    semaphore_signal(&g_sem_fifo);

    /* Anti-barging check: count is still 0, new contender try_wait must fail */
    if (semaphore_get_count(&g_sem_fifo) != 0) {
        panic("3: Semaphore count incremented during direct handoff!");
    }
    int sem_barge = semaphore_try_wait(&g_sem_fifo);
    if (sem_barge != SEMAPHORE_ERR_BUSY) {
        panic("3: Anti-barging violation! Permitted stealing of handed-off semaphore permit!");
    }

    /* Wait for s1 to receive its permit, log, and exit */
    while (s1->state != THREAD_EXITED) {
        thread_yield();
    }
    if (g_sem_fifo.waiters.count != 1) panic("3: Waiter 2 not retained in wait queue!");

    /* Signal second time: releases s2 */
    semaphore_signal(&g_sem_fifo);

    while (s2->state != THREAD_EXITED || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_sem_fifo_log_idx != 2 || g_sem_fifo_log[0] != 10 || g_sem_fifo_log[1] != 20) {
        panic("3: Semaphore FIFO order mismatch! [%d, %d]", g_sem_fifo_log[0], g_sem_fifo_log[1]);
    }
    kprintf("  PASS: 3. Semaphore permit accounting, overflow rejection, and FIFO anti-barging verified.\n");
    kprintf("ASTRA_SEMAPHORE_OK\n\n");

    /* ---------------------------------------------------------------------
     * 4. Semaphore Maskable IRQ Signaling
     * --------------------------------------------------------------------- */
    kprintf("[SYNC TEST] 4. Semaphore maskable IRQ-to-thread signaling...\n");

    semaphore_init(&g_sem_irq, 0, 5);
    g_sem_irq_hook_ran = false;
    g_worker_irq_done = false;

    thread_t *w_irq = thread_create("sem_irq_w", worker_sem_irq_waiter, NULL);
    if (!w_irq) panic("4: Failed to create IRQ waiter thread!");

    while (w_irq->state != THREAD_BLOCKED) {
        thread_yield();
    }

    /* Register test hook in timer IRQ handler */
    sched_set_timer_irq_hook(timer_irq_signaler_hook, &g_sem_irq);

    /* Spin while interrupts are enabled: timer will deliver IRQ and signal semaphore */
    u64 t_start = apic_get_ticks();
    while (!g_worker_irq_done) {
        if (apic_get_ticks() > t_start + 100) {
            panic("4: IRQ-to-thread semaphore signal timed out!");
        }
        __asm__ volatile ("pause");
    }

    while (w_irq->state != THREAD_EXITED || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (!g_sem_irq_hook_ran) panic("4: Timer IRQ signaler hook was never invoked!");
    kprintf("  PASS: 4. Semaphore signaled safely from hardware IRQ and awakened blocked thread.\n");
    kprintf("ASTRA_SEMAPHORE_IRQ_OK\n\n");

    /* ---------------------------------------------------------------------
     * 5. Producer-Consumer Bounded Buffer Stress Test
     * --------------------------------------------------------------------- */
    kprintf("[SYNC TEST] 5. Producer-Consumer bounded buffer stress test (2000 items + preemption)...\n");

    g_buf_in = 0;
    g_buf_out = 0;
    g_total_produced_sum = 0;
    g_total_consumed_sum = 0;
    g_producers_done = 0;
    g_consumers_done = 0;
    g_spinner_stop = false;

    mutex_init(&g_buf_mutex);
    semaphore_init(&g_sem_empty, BUFFER_CAPACITY, BUFFER_CAPACITY);
    semaphore_init(&g_sem_full, 0, BUFFER_CAPACITY);

    /* Start CPU-bound spinner thread to generate frequent timer preemptions */
    thread_t *spinner = thread_create("stress_spin", worker_stress_spinner, NULL);
    if (!spinner) panic("5: Failed to create spinner thread!");

    /* Start 2 producers and 2 consumers */
    thread_t *p1 = thread_create("prod1", worker_stress_producer, (void *)(uintptr_t)0);
    thread_t *p2 = thread_create("prod2", worker_stress_producer, (void *)(uintptr_t)10000);
    thread_t *con1 = thread_create("cons1", worker_stress_consumer, NULL);
    thread_t *con2 = thread_create("cons2", worker_stress_consumer, NULL);
    if (!p1 || !p2 || !con1 || !con2) panic("5: Failed to create producer/consumer threads!");

    u64 stress_timeout = apic_get_ticks() + 1000;
    while (g_producers_done < 2 || g_consumers_done < 2) {
        if (apic_get_ticks() > stress_timeout) {
            panic("5: Producer-Consumer stress test deadlocked! prod=%d, cons=%d",
                  g_producers_done, g_consumers_done);
        }
        thread_yield();
    }

    g_spinner_stop = true;

    while (spinner->state != THREAD_EXITED || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_total_produced_sum != g_total_consumed_sum || g_total_produced_sum == 0) {
        panic("5: Data corruption in bounded buffer! produced=%llu, consumed=%llu",
              (unsigned long long)g_total_produced_sum, (unsigned long long)g_total_consumed_sum);
    }
    if (g_buf_in != g_buf_out) {
        panic("5: Buffer indices out of sync: in=%d, out=%d", g_buf_in, g_buf_out);
    }
    if (semaphore_get_count(&g_sem_empty) != BUFFER_CAPACITY || semaphore_get_count(&g_sem_full) != 0) {
        panic("5: Semaphore permit imbalance at completion: empty=%u, full=%u",
              semaphore_get_count(&g_sem_empty), semaphore_get_count(&g_sem_full));
    }

    /* Resource accounting validation */
    struct pmm_stats pmm_after;
    pmm_get_stats(&pmm_after);
    struct heap_stats heap_after;
    heap_get_stats(&heap_after);

    if (pmm_before.free_pages != pmm_after.free_pages) {
        panic("5: PMM frame leak across synchronization self-tests! Before: %llu, After: %llu",
              (unsigned long long)pmm_before.free_pages, (unsigned long long)pmm_after.free_pages);
    }
    if (heap_before.live_allocations != heap_after.live_allocations) {
        panic("5: Heap allocation leak across synchronization self-tests! Before: %llu, After: %llu",
              (unsigned long long)heap_before.live_allocations, (unsigned long long)heap_after.live_allocations);
    }

    kprintf("  PASS: 5. 2000 items transferred across bounded buffer under preemption; 0 deadlocks, 0 leaks.\n");
    kprintf("ASTRA_SYNC_STRESS_OK\n\n");
    kprintf("==================================================================\n\n");
}
#endif /* !HOST_TEST */
