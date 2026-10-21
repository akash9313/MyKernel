#ifndef ASTRA_KERNEL_SYNC_H
#define ASTRA_KERNEL_SYNC_H

#include <kernel/types.h>
#include <kernel/sched.h>

/*
 * AstraOS Synchronization Architecture: Mutexes & Counting Semaphores
 *
 * Scope: Single-CPU Kernel Synchronization
 *
 * Synchronization Contracts:
 * 1. Storage & Allocation:
 *    - Synchronization objects (mutex_t, semaphore_t) are caller-owned.
 *    - Acquisition, try-acquisition, release, and IRQ-signal paths perform ZERO
 *      dynamic memory allocations (no kmalloc/kfree).
 * 2. Object Lifetime:
 *    - An object must outlive all waiters, owners, and in-flight operations.
 *    - Reinitialization or dynamic destruction while in use is strictly prohibited.
 * 3. Calling Context Restrictions:
 *    - mutex_init / semaphore_init: Thread context or early bootstrap.
 *    - mutex_lock / semaphore_wait:
 *        Thread context only. CAN BLOCK.
 *        Forbidden in interrupt (in_interrupt() == true), exception, NMI, or panic contexts.
 *        Forbidden if preemption is disabled (preempt_count() > 0) or interrupts are disabled (IF == 0).
 *    - mutex_try_lock / semaphore_try_wait:
 *        Thread context. Non-blocking.
 *    - mutex_unlock:
 *        Thread context only. Must be called by the current owner.
 *        Non-owner unlock is a diagnosed fatal programming error.
 *    - semaphore_signal:
 *        Thread context OR ordinary maskable IRQ context (bounded, allocation-free, scheduler-safe).
 *        Strictly prohibited in NMI or CPU exception contexts.
 *        When called from IRQ context, scheduling directly is prohibited;
 *        the awakened waiter is marked READY, and rescheduling is deferred to the IRQ exit boundary.
 * 4. Direct Handoff & Anti-Barging:
 *    - Mutex: On unlock with waiters, ownership is assigned directly to the FIFO head waiter
 *      (m->owner = next_owner) before the waiter is added to the ready queue. New contenders
 *      see m->owner != NULL and cannot steal the lock (anti-barging).
 *    - Semaphore: On signal with waiters, the permit is transferred directly to the FIFO head waiter
 *      without incrementing s->count. New contenders see s->count == 0 and cannot steal the permit.
 * 5. Preemption Safety:
 *    - Preemption is NOT held disabled while holding a mutex. Mutexes exist to protect data
 *      while allowing normal preemption and thread blocking.
 *    - Internal atomic mutations use brief local interrupt masking (cli / sti) to protect
 *      scheduler wait-queue state.
 * 6. Thread Exit Safety:
 *    - A thread attempting to exit while owning mutexes (curr->mutex_count > 0) triggers an immediate panic.
 */

#define MUTEX_OK                 0
#define MUTEX_ERR_INVALID       (-1)
#define MUTEX_ERR_BUSY          (-2)
#define MUTEX_ERR_RECURSIVE     (-3)
#define MUTEX_ERR_NOT_OWNER     (-4)
#define MUTEX_ERR_CONTEXT       (-5)

#define SEMAPHORE_OK             0
#define SEMAPHORE_ERR_INVALID   (-1)
#define SEMAPHORE_ERR_BUSY      (-2)
#define SEMAPHORE_ERR_OVERFLOW  (-3)
#define SEMAPHORE_ERR_CONTEXT   (-4)

/* Non-recursive Kernel Mutex */
typedef struct mutex {
    thread_t *owner;        /* Current owning thread, or NULL if free */
    wait_queue_t waiters;   /* FIFO queue of waiting threads */
    bool initialized;       /* Initialization sentinel */
} mutex_t;

/* Bounded Counting Semaphore */
typedef struct semaphore {
    u32 count;              /* Current available permits */
    u32 maximum_count;      /* Upper bound capacity (> 0) */
    wait_queue_t waiters;   /* FIFO queue of waiting threads */
    bool initialized;       /* Initialization sentinel */
} semaphore_t;

/* Mutex API */
int mutex_init(mutex_t *m);
int mutex_lock(mutex_t *m);
int mutex_try_lock(mutex_t *m);
int mutex_unlock(mutex_t *m);
bool mutex_is_locked(const mutex_t *m);
thread_t *mutex_get_owner(const mutex_t *m);

/* Semaphore API */
int semaphore_init(semaphore_t *s, u32 initial_count, u32 maximum_count);
int semaphore_wait(semaphore_t *s);
int semaphore_try_wait(semaphore_t *s);
int semaphore_signal(semaphore_t *s);
u32 semaphore_get_count(const semaphore_t *s);

/* Test suite */
void sched_run_sync_tests(void);

#endif /* ASTRA_KERNEL_SYNC_H */
