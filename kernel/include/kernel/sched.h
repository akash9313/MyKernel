#ifndef ASTRA_KERNEL_SCHED_H
#define ASTRA_KERNEL_SCHED_H

#include <kernel/types.h>
#include <kernel/deadline.h>

typedef u32 tid_t;

/*
 * Thread State Machine:
 *   THREAD_EMBRYO:  Newly allocated, stack initialized, not yet runnable.
 *   THREAD_READY:   Runnable and queued in g_ready_head.
 *   THREAD_RUNNING: Currently executing on CPU.
 *   THREAD_BLOCKED: Awaiting an explicit event, wait queue, or timer deadline. Not in ready queue.
 *   THREAD_EXITED:  Terminated, queued on g_reap_head awaiting deferred reclamation.
 *
 * Legal Transitions:
 *   EMBRYO  -> READY    (on thread_create)
 *   READY   -> RUNNING  (on scheduler context switch)
 *   RUNNING -> READY    (on cooperative thread_yield)
 *   RUNNING -> BLOCKED  (on sched_block_current while waiting for event or sleep)
 *   BLOCKED -> READY    (on explicit event_signal or timer deadline expiration)
 *   RUNNING -> EXITED   (on thread_exit)
 */
typedef enum {
    THREAD_EMBRYO = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_EXITED
} thread_state_t;

#include <kernel/preempt.h>

#define THREAD_STACK_PAGES      4                   /* 16 KiB stack body */
#define THREAD_STACK_SIZE       (THREAD_STACK_PAGES * 4096)
#define THREAD_MAX_THREADS      32                  /* Bounded thread capacity */
#define THREAD_STACK_SLOT_SIZE  (5 * 4096)          /* 1 guard page (4 KiB) + 4 body pages (16 KiB) = 20 KiB */
#define THREAD_STACKS_BASE_VA   0xffffffff80800000ULL

#define SLEEP_QUEUE_CAPACITY    16                  /* Explicit bounded capacity for active sleepers */

/* Return and error codes for wait and sleep operations */
#define WAIT_OK                 0
#define WAIT_ERR_UNSUPPORTED    (-1)
#define WAIT_ERR_INVALID_STATE  (-2)

#define SLEEP_OK                0
#define SLEEP_ERR_INVALID_DUR   (-1)
#define SLEEP_ERR_QUEUE_FULL    (-2)
#define SLEEP_ERR_UNSUPPORTED   (-3)
#define SLEEP_ERR_INVALID_STATE (-4)

struct thread {
    tid_t tid;
    thread_state_t state;
    void *saved_rsp;

    /* Preemption safety & rescheduling tracking (per-thread) */
    u32 preempt_count;
    bool need_resched;

    /* Stack allocation & mapping ownership */
    int slot_index;
    virt_addr_t guard_va;
    virt_addr_t stack_bottom;
    virt_addr_t stack_top;
    phys_addr_t stack_pas[THREAD_STACK_PAGES];
    size_t stack_page_count;
    bool is_boot_thread;

    /* Entry function and argument */
    void (*entry)(void *);
    void *arg;

    char name[32];

    /* Queue membership tracking & invariants */
    bool in_ready_queue;
    bool in_wait_queue;
    bool in_timer_queue;

    /* Waiter ownership, reason, and wake result */
    const char *wait_reason;
    void *wait_channel;
    int wake_result;

    /* Sleep deadline tracking */
    u64 sleep_deadline;
    u64 sleep_insert_seq;

    /* Synchronization ownership tracking */
    u32 mutex_count;

    /* Associated process (NULL for pure kernel threads) */
    struct process *process;

    /* Separate intrusive linkages for each distinct queue */
    struct thread *ready_next;
    struct thread *wait_next;
    struct thread *timer_next;
    struct thread *reap_next;
};

struct process;
typedef struct thread thread_t;

/* FIFO Wait Queue */
typedef struct wait_queue {
    thread_t *head;
    thread_t *tail;
    size_t count;
} wait_queue_t;

void wait_queue_init(wait_queue_t *wq);
void wait_queue_enqueue(wait_queue_t *wq, thread_t *t);
thread_t *wait_queue_dequeue(wait_queue_t *wq);
bool wait_queue_remove(wait_queue_t *wq, thread_t *t);
size_t wait_queue_count(const wait_queue_t *wq);

/* Scheduler Ready Queue Operations */
void sched_enqueue(thread_t *t);

/*
 * Event Destruction Contract:
 * event_destroy() requires external quiescence: all potential external callers
 * and IRQ signalers must have ceased accessing the event before destruction is invoked.
 * An empty waiter queue and clean ready-queue inspection are internal assertions,
 * but do not guarantee safety if external callers retain dangling pointers. Event lifetimes
 * must be externally managed and synchronized before destruction.
 */

/* Persistent Auto-Reset Event Primitive */
typedef struct event {
    bool signaled;          /* Stored pending signal (coalesced) */
    wait_queue_t waiters;   /* FIFO queue of waiting threads */
    bool initialized;
} event_t;

void event_init(event_t *ev, bool initial_signaled);
int event_wait(event_t *ev);
void event_signal(event_t *ev);
void event_destroy(event_t *ev);

/* Public Threading & Scheduler API */
void sched_init(void);
thread_t *thread_create(const char *name, void (*entry)(void *), void *arg);
void thread_yield(void);
void thread_exit(void) __attribute__((noreturn));
thread_t *thread_current(void);
size_t sched_ready_count(void);
size_t sched_sleep_count(void);
void sched_reap(void);
bool sched_has_ready_threads(void);
bool sched_is_initialized(void);

/* Tick-based Sleep API */
int thread_sleep_ticks(u64 delta);

/* Timer IRQ Integration & Test Hooks */
void sched_timer_tick(u64 current_ticks);
void sched_set_timer_irq_hook(void (*hook)(void *), void *arg);

/* Phase C: IRQ-exit preemption hook (called from irq_common_stub after interrupt_handler returns) */
void sched_try_preempt_irq_exit(u64 interrupted_rflags);

/* Internal IRQ-disabled blocking primitive */
void sched_block_current(u64 rflags, const char *reason, void *channel);

/* Interrupt context tracking */
extern volatile int g_interrupt_depth;

/* In-kernel threading test suites */
void sched_run_self_tests(void);
void sched_run_preemption_tests(void);

#endif /* ASTRA_KERNEL_SCHED_H */
