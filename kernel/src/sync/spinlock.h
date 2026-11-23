#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Basic CPU spinlock.
 *
 * value:
 *   0 = unlocked
 *   1 = locked
 *
 * initialized:
 *   Indicates that spinlock_init() has completed.
 *
 * Initialize a lock before publishing it to other execution
 * contexts. Do not reinitialize a lock while it is in use.
 */
typedef struct spinlock {
    uint32_t value;
    bool initialized;
} spinlock_t;


/*
 * Initialize a spinlock.
 */
bool spinlock_init(spinlock_t *lock);


/*
 * Return whether the lock is currently held.
 */
bool spinlock_is_locked(const spinlock_t *lock);


/*
 * Acquire a spinlock.
 *
 * Prevents involuntary preemption while the lock is held.
 * Does not disable interrupts.
 */
bool spinlock_lock(spinlock_t *lock);


/*
 * Release a spinlock acquired with spinlock_lock().
 */
bool spinlock_unlock(spinlock_t *lock);


/*
 * Acquire a spinlock while saving and disabling local
 * maskable interrupts.
 *
 * The original interrupt flags are written to flags_out.
 */
bool spinlock_lock_irqsave(
    spinlock_t *lock,
    uint64_t *flags_out
);


/*
 * Release a lock acquired with spinlock_lock_irqsave()
 * and restore its original interrupt state.
 */
bool spinlock_unlock_irqrestore(
    spinlock_t *lock,
    uint64_t flags
);

#endif
