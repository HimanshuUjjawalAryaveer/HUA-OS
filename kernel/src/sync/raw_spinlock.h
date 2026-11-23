
#ifndef RAW_SPINLOCK_H
#define RAW_SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Scheduler-independent atomic spinlock.
 *
 * value:
 *   0 = unlocked
 *   1 = locked
 *
 * initialized:
 *   Set after raw_spinlock_init() completes.
 *
 * This lock:
 * - Disables local maskable interrupts while held.
 * - Uses atomic operations for cross-CPU exclusion.
 * - Does not access scheduler or thread state.
 * - Can be used during early kernel initialization.
 *
 * The lock is non-recursive.
 * Never acquire the same lock twice without releasing it.
 * Never use it from an NMI handler that might interrupt
 * code already holding the same lock.
 */
typedef struct raw_spinlock {

    uint32_t value;

    bool initialized;

} raw_spinlock_t;


/*
 * Initialize a zero-initialized raw spinlock.
 *
 * Initialize before publishing the lock to other CPUs.
 * Do not reinitialize a lock while it is in use.
 */
bool raw_spinlock_init(raw_spinlock_t *lock);


/*
 * Acquire a raw spinlock and disable local interrupts.
 *
 * The caller must provide storage for the original RFLAGS.
 */
bool raw_spinlock_lock_irqsave(
    raw_spinlock_t *lock,
    uint64_t *flags_out
);


/*
 * Release a raw spinlock and restore the original
 * local interrupt state.
 */
bool raw_spinlock_unlock_irqrestore(
    raw_spinlock_t *lock,
    uint64_t flags
);


/*
 * Check whether the lock is currently held.
 */
bool raw_spinlock_is_locked(const raw_spinlock_t *lock);

#endif
