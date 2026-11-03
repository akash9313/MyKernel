#ifndef ASTRA_KERNEL_SPINLOCK_H
#define ASTRA_KERNEL_SPINLOCK_H

#include <kernel/types.h>
#include <kernel/arch/x86_64/cpu.h>

/* =========================================================================
 * KERNEL SPINLOCK PRIMITIVES (SMP)
 * ========================================================================= */

typedef struct spinlock {
    volatile u32 locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock_t *lock) {
    if (lock) {
        lock->locked = 0;
    }
}

static inline void spinlock_acquire(spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        cpu_pause();
    }
}

static inline bool spinlock_try_acquire(spinlock_t *lock) {
    return !__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE);
}

static inline void spinlock_release(spinlock_t *lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

static inline u64 spinlock_acquire_irqsave(spinlock_t *lock) {
    u64 flags = irq_save();
    spinlock_acquire(lock);
    return flags;
}

static inline void spinlock_release_irqrestore(spinlock_t *lock, u64 flags) {
    spinlock_release(lock);
    irq_restore(flags);
}

#endif /* ASTRA_KERNEL_SPINLOCK_H */

