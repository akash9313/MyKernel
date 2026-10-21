#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>

#include <kernel/types.h>
#include <kernel/sync.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>

/* Mock / Panic Harness */
static jmp_buf g_panic_jmp;
static bool g_expect_panic = false;
static int g_panics_caught = 0;
static char g_last_panic_msg[256] = {0};

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void kernel_panic(const char *file, int line, const char *fmt, ...) {
    (void)file;
    (void)line;
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_panic_msg, sizeof(g_last_panic_msg), fmt, args);
    va_end(args);
    printf("[PANIC MSG: %s]\n", g_last_panic_msg);
    fflush(stdout);

    if (g_expect_panic) {
        g_panics_caught++;
        longjmp(g_panic_jmp, 1);
    } else {
        fprintf(stderr, "\n[FATAL PANIC at %s:%d] %s\n", file, line, g_last_panic_msg);
        abort();
    }
}

/* Mock Thread & Context State */
static thread_t g_host_thread_1;
static thread_t g_host_thread_2;
static thread_t g_host_thread_3;
static thread_t *g_host_current_thread = &g_host_thread_1;

static u32 g_host_preempt_count = 0;
static bool g_host_in_irq = false;
static bool g_host_in_exception = false;
static bool g_host_if = true;

/* Stubs for sched/preempt/arch queried by sync.c */
thread_t *thread_current(void) {
    return g_host_current_thread;
}

u32 preempt_count(void) {
    return g_host_preempt_count;
}

bool in_interrupt(void) {
    return g_host_in_irq || g_host_in_exception;
}

bool in_irq(void) {
    return g_host_in_irq;
}

bool in_exception(void) {
    return g_host_in_exception;
}

static u64 g_host_ticks = 0;
u64 apic_get_ticks(void) { return ++g_host_ticks; }
void pmm_get_stats(struct pmm_stats *out) { if (out) memset(out, 0, sizeof(*out)); }
void heap_get_stats(struct heap_stats *out) { if (out) memset(out, 0, sizeof(*out)); }

static thread_t *g_host_ready_head = NULL;
static thread_t *g_host_ready_tail = NULL;
static bool g_host_need_resched = false;

void sched_enqueue(thread_t *t) {
    assert(t != NULL);
    t->ready_next = NULL;
    t->in_ready_queue = true;
    if (!g_host_ready_head) {
        g_host_ready_head = t;
        g_host_ready_tail = t;
    } else {
        g_host_ready_tail->ready_next = t;
        g_host_ready_tail = t;
    }
}

void set_need_resched(void) {
    g_host_need_resched = true;
}

bool need_resched(void) {
    return g_host_need_resched;
}

bool scheduling_permitted(void) {
    return (!in_interrupt() && (preempt_count() == 0) && g_host_if);
}

void sched_reschedule(void) {
    g_host_need_resched = false;
}

void sched_block_current(u64 rflags, const char *reason, void *channel) {
    (void)rflags;
    (void)reason;
    (void)channel;
    g_host_current_thread->state = THREAD_BLOCKED;
}

void wait_queue_init(wait_queue_t *wq) {
    assert(wq != NULL);
    wq->head = NULL;
    wq->tail = NULL;
    wq->count = 0;
}

void wait_queue_enqueue(wait_queue_t *wq, thread_t *t) {
    assert(wq != NULL && t != NULL);
    if (t->in_wait_queue) {
        kernel_panic("test", 0, "wait_queue_enqueue: Thread TID %u already registered in wait queue!", t->tid);
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
    assert(wq != NULL);
    if (!wq->head) return NULL;
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

/* Include sync implementation directly into test translation unit */
#include "../kernel/sched/sync.c"

int main(void) {
    printf("======================================================\n");
    printf("       AstraOS Mutex & Semaphore Host Tests           \n");
    printf("======================================================\n");

    /* Initialize Mock Threads */
    memset(&g_host_thread_1, 0, sizeof(thread_t));
    g_host_thread_1.tid = 1;
    strcpy(g_host_thread_1.name, "host_thread_1");

    memset(&g_host_thread_2, 0, sizeof(thread_t));
    g_host_thread_2.tid = 2;
    strcpy(g_host_thread_2.name, "host_thread_2");

    memset(&g_host_thread_3, 0, sizeof(thread_t));
    g_host_thread_3.tid = 3;
    strcpy(g_host_thread_3.name, "host_thread_3");

    /* 1. Mutex Initialization & Uncontended Lock/Unlock */
    printf("[HOST TEST] 1. Mutex initialization and uncontended lock/unlock...\n");
    {
        mutex_t m;
        assert(mutex_init(&m) == MUTEX_OK);
        assert(!mutex_is_locked(&m));
        assert(mutex_get_owner(&m) == NULL);

        assert(mutex_lock(&m) == MUTEX_OK);
        assert(mutex_is_locked(&m));
        assert(mutex_get_owner(&m) == &g_host_thread_1);
        assert(g_host_thread_1.mutex_count == 1);

        assert(mutex_unlock(&m) == MUTEX_OK);
        assert(!mutex_is_locked(&m));
        assert(mutex_get_owner(&m) == NULL);
        assert(g_host_thread_1.mutex_count == 0);
        printf("  PASS: Uncontended lock and unlock verified.\n");
    }

    /* 2. Mutex Try-Lock Semantics */
    printf("[HOST TEST] 2. Mutex try-lock semantics...\n");
    {
        mutex_t m;
        mutex_init(&m);

        assert(mutex_try_lock(&m) == MUTEX_OK);
        assert(mutex_is_locked(&m));
        assert(g_host_thread_1.mutex_count == 1);

        /* Try lock while owned by self returns MUTEX_ERR_RECURSIVE */
        assert(mutex_try_lock(&m) == MUTEX_ERR_RECURSIVE);

        /* Switch thread to thread 2: try-lock returns MUTEX_ERR_BUSY */
        g_host_current_thread = &g_host_thread_2;
        assert(mutex_try_lock(&m) == MUTEX_ERR_BUSY);

        /* Switch back and unlock */
        g_host_current_thread = &g_host_thread_1;
        assert(mutex_unlock(&m) == MUTEX_OK);

        /* Thread 2 can now try-lock */
        g_host_current_thread = &g_host_thread_2;
        assert(mutex_try_lock(&m) == MUTEX_OK);
        assert(mutex_get_owner(&m) == &g_host_thread_2);
        assert(mutex_unlock(&m) == MUTEX_OK);

        g_host_current_thread = &g_host_thread_1;
        printf("  PASS: Mutex try-lock semantics verified.\n");
    }

    /* 3. Mutex Diagnostics: Non-Owner Unlock Trap */
    printf("[HOST TEST] 3. Mutex non-owner unlock diagnosed trap...\n");
    {
        mutex_t m;
        mutex_init(&m);
        mutex_lock(&m); /* Thread 1 is owner */

        g_host_current_thread = &g_host_thread_2;
        g_expect_panic = true;
        if (setjmp(g_panic_jmp) == 0) {
            mutex_unlock(&m); /* Should panic: thread 2 unlocking thread 1's mutex */
            assert(false && "Non-owner unlock failed to panic!");
        } else {
            /* Panic caught */
            assert(strstr(g_last_panic_msg, "attempted to unlock mutex") != NULL &&
                   strstr(g_last_panic_msg, "owned by") != NULL);
        }
        g_expect_panic = false;

        /* Clean up */
        g_host_current_thread = &g_host_thread_1;
        mutex_unlock(&m);
        printf("  PASS: Non-owner unlock cleanly trapped and diagnosed.\n");
    }

    /* 4. Mutex Diagnostics: Recursive Lock Trap */
    printf("[HOST TEST] 4. Mutex recursive lock diagnosed trap...\n");
    {
        mutex_t m;
        mutex_init(&m);
        mutex_lock(&m);

        g_expect_panic = true;
        if (setjmp(g_panic_jmp) == 0) {
            mutex_lock(&m); /* Should panic: recursive lock */
            assert(false && "Recursive lock failed to panic!");
        } else {
            assert(strstr(g_last_panic_msg, "recursive acquisition attempted") != NULL);
        }
        g_expect_panic = false;

        mutex_unlock(&m);
        printf("  PASS: Recursive lock cleanly trapped and diagnosed.\n");
    }

    /* 5. Mutex Direct Ownership Handoff & Anti-Barging */
    printf("[HOST TEST] 5. Mutex direct ownership handoff and anti-barging...\n");
    {
        mutex_t m;
        mutex_init(&m);

        /* Thread 1 acquires lock */
        mutex_lock(&m);

        /* Thread 2 and Thread 3 are registered as waiters */
        wait_queue_enqueue(&m.waiters, &g_host_thread_2);
        wait_queue_enqueue(&m.waiters, &g_host_thread_3);

        /* Thread 1 unlocks: direct handoff to Thread 2 */
        mutex_unlock(&m);

        /* Owner is immediately Thread 2! */
        assert(m.owner == &g_host_thread_2);
        assert(g_host_thread_2.mutex_count == 1);
        assert(m.waiters.count == 1);

        /* Anti-barging check: Thread 1 (or any new contender) try-lock fails */
        assert(mutex_try_lock(&m) == MUTEX_ERR_BUSY);

        /* Thread 2 runs and unlocks: direct handoff to Thread 3 */
        g_host_current_thread = &g_host_thread_2;
        mutex_unlock(&m);

        assert(m.owner == &g_host_thread_3);
        assert(g_host_thread_3.mutex_count == 1);
        assert(m.waiters.count == 0);

        /* Thread 3 unlocks: mutex becomes free */
        g_host_current_thread = &g_host_thread_3;
        mutex_unlock(&m);
        assert(m.owner == NULL);

        g_host_current_thread = &g_host_thread_1;
        printf("  PASS: Mutex direct handoff and anti-barging verified.\n");
    }

    /* 6. Semaphore Initialization and Boundary Checks */
    printf("[HOST TEST] 6. Semaphore initialization and boundaries...\n");
    {
        semaphore_t s;
        assert(semaphore_init(&s, 0, 0) == SEMAPHORE_ERR_INVALID);
        assert(semaphore_init(&s, 5, 2) == SEMAPHORE_ERR_INVALID);
        assert(semaphore_init(&s, 3, 5) == SEMAPHORE_OK);
        assert(semaphore_get_count(&s) == 3);
        printf("  PASS: Semaphore parameter validation verified.\n");
    }

    /* 7. Semaphore Try-Wait & Capacity Overflow Rejection */
    printf("[HOST TEST] 7. Semaphore permit accounting & overflow rejection...\n");
    {
        semaphore_t s;
        semaphore_init(&s, 2, 4);

        assert(semaphore_try_wait(&s) == SEMAPHORE_OK);
        assert(semaphore_get_count(&s) == 1);
        assert(semaphore_try_wait(&s) == SEMAPHORE_OK);
        assert(semaphore_get_count(&s) == 0);
        assert(semaphore_try_wait(&s) == SEMAPHORE_ERR_BUSY);

        assert(semaphore_signal(&s) == SEMAPHORE_OK);
        assert(semaphore_signal(&s) == SEMAPHORE_OK);
        assert(semaphore_signal(&s) == SEMAPHORE_OK);
        assert(semaphore_signal(&s) == SEMAPHORE_OK);
        assert(semaphore_get_count(&s) == 4);

        /* 5th signal overflows max capacity (4): returns error, leaves count unchanged */
        assert(semaphore_signal(&s) == SEMAPHORE_ERR_OVERFLOW);
        assert(semaphore_get_count(&s) == 4);
        printf("  PASS: Semaphore accounting and overflow rejection verified.\n");
    }

    /* 8. Semaphore Direct Permit Handoff & Anti-Barging */
    printf("[HOST TEST] 8. Semaphore direct permit handoff & anti-barging...\n");
    {
        semaphore_t s;
        semaphore_init(&s, 0, 5);

        /* Enqueue Thread 2 as waiter */
        wait_queue_enqueue(&s.waiters, &g_host_thread_2);
        assert(s.waiters.count == 1);

        /* Signal from Thread 1: directly transfers permit to Thread 2 */
        assert(semaphore_signal(&s) == SEMAPHORE_OK);

        /* Count is NOT incremented; permit went directly to waiter */
        assert(semaphore_get_count(&s) == 0);

        /* Anti-barging: Thread 1 cannot steal the permit */
        assert(semaphore_try_wait(&s) == SEMAPHORE_ERR_BUSY);

        /* Thread 2 was marked READY */
        assert(g_host_thread_2.wake_result == SEMAPHORE_OK);
        printf("  PASS: Semaphore direct handoff and anti-barging verified.\n");
    }

    /* 9. Semaphore Maskable IRQ Signaling */
    printf("[HOST TEST] 9. Semaphore maskable IRQ signaling...\n");
    {
        semaphore_t s;
        semaphore_init(&s, 0, 5);

        /* Simulate IRQ context */
        g_host_in_irq = true;
        assert(semaphore_signal(&s) == SEMAPHORE_OK);
        g_host_in_irq = false;

        assert(semaphore_get_count(&s) == 1);
        assert(semaphore_try_wait(&s) == SEMAPHORE_OK);
        assert(semaphore_get_count(&s) == 0);

        /* Simulate Exception context: signal must panic */
        g_host_in_exception = true;
        g_expect_panic = true;
        if (setjmp(g_panic_jmp) == 0) {
            semaphore_signal(&s);
            assert(false && "Signal from exception context failed to panic!");
        } else {
            assert(strstr(g_last_panic_msg, "forbidden in exception/NMI context") != NULL);
        }
        g_expect_panic = false;
        g_host_in_exception = false;
        printf("  PASS: Maskable IRQ signal permitted; exception context rejected.\n");
    }

    /* 10. Context Rejection: Nested Preemption Guards & Interrupt Context */
    printf("[HOST TEST] 10. Nested preemption guards & interrupt-context rejection...\n");
    {
        mutex_t m;
        mutex_init(&m);
        semaphore_t s;
        semaphore_init(&s, 1, 1);

        /* Preemption disabled -> mutex_lock must panic */
        g_host_preempt_count = 1;
        g_expect_panic = true;
        if (setjmp(g_panic_jmp) == 0) {
            mutex_lock(&m);
            assert(false && "mutex_lock while preempt_count > 0 failed to panic!");
        } else {
            assert(strstr(g_last_panic_msg, "forbidden while holding preemption-disabled") != NULL);
        }

        /* Preemption disabled -> semaphore_wait must panic */
        if (setjmp(g_panic_jmp) == 0) {
            semaphore_wait(&s);
            assert(false && "semaphore_wait while preempt_count > 0 failed to panic!");
        } else {
            assert(strstr(g_last_panic_msg, "forbidden while holding preemption-disabled") != NULL);
        }
        g_expect_panic = false;
        g_host_preempt_count = 0;

        /* In IRQ context -> mutex_lock must panic */
        g_host_in_irq = true;
        g_expect_panic = true;
        if (setjmp(g_panic_jmp) == 0) {
            mutex_lock(&m);
            assert(false && "mutex_lock in IRQ context failed to panic!");
        } else {
            assert(strstr(g_last_panic_msg, "forbidden in interrupt context") != NULL);
        }

        /* In IRQ context -> semaphore_wait must panic */
        if (setjmp(g_panic_jmp) == 0) {
            semaphore_wait(&s);
            assert(false && "semaphore_wait in IRQ context failed to panic!");
        } else {
            assert(strstr(g_last_panic_msg, "forbidden in interrupt context") != NULL);
        }
        g_expect_panic = false;
        g_host_in_irq = false;
        printf("  PASS: Blocking operations strictly rejected in IRQ & preemption-disabled contexts.\n");
    }

    /* 11. Thread Exit with Owned Mutexes Diagnostic */
    printf("[HOST TEST] 11. Thread exit while owning mutex diagnosed panic...\n");
    {
        mutex_t m;
        mutex_init(&m);
        mutex_lock(&m);

        assert(g_host_current_thread->mutex_count == 1);
        g_expect_panic = true;
        if (setjmp(g_panic_jmp) == 0) {
            if (g_host_current_thread->mutex_count > 0) {
                panic("thread_exit: Thread TID %u attempted to exit while still owning %u mutex(es)!",
                      g_host_current_thread->tid, g_host_current_thread->mutex_count);
            }
            assert(false && "thread_exit with held mutex failed to panic!");
        } else {
            assert(strstr(g_last_panic_msg, "attempted to exit while still owning") != NULL);
        }
        g_expect_panic = false;

        mutex_unlock(&m);
        assert(g_host_current_thread->mutex_count == 0);
        printf("  PASS: Thread exit while owning mutex cleanly trapped and diagnosed.\n");
    }

    printf("------------------------------------------------------\n");
    printf("[SUCCESS] All host-side Mutex and Semaphore tests passed!\n");
    printf("======================================================\n");
    return 0;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int run_all_sync_tests(void) {
    return main();
}
