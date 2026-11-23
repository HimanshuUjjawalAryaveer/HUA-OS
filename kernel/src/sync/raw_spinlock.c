
#include "sync/raw_spinlock.h"

#include "arch/x86_64/irq_flags.h"

#include <stddef.h>


/*
 * CPU pause instruction.
 *
 * Reduces resource consumption while waiting for
 * another execution context to release the lock.
 */
static inline void raw_spinlock_cpu_relax(void) {

    asm volatile ("pause" ::: "memory");
}


/*
 * Check whether a lock has been initialized.
 */
static bool raw_spinlock_is_valid(
    const raw_spinlock_t *lock
) {

    return lock != NULL &&
        __atomic_load_n(
            &lock->initialized,
            __ATOMIC_ACQUIRE
        );
}


/*
 * Initialize a raw spinlock.
 */
bool raw_spinlock_init(raw_spinlock_t *lock) {

    if (lock == NULL) {
        return false;
    }

    /*
     * Prevent accidental reinitialization.
     */
    if (__atomic_load_n(
            &lock->initialized,
            __ATOMIC_ACQUIRE
        )) {

        return false;
    }

    __atomic_store_n(
        &lock->value,
        0,
        __ATOMIC_RELAXED
    );

    __atomic_store_n(
        &lock->initialized,
        true,
        __ATOMIC_RELEASE
    );

    return true;
}


/*
 * Acquire a raw spinlock.
 *
 * Local interrupts are disabled before attempting acquisition.
 *
 * This prevents a local interrupt handler from interrupting
 * the lock owner and spinning forever on the same lock.
 *
 * No scheduler operation is performed.
 */
bool raw_spinlock_lock_irqsave(
    raw_spinlock_t *lock,
    uint64_t *flags_out
) {

    if (!raw_spinlock_is_valid(lock) ||
        flags_out == NULL) {

        return false;
    }

    /*
     * Preserve the caller's original interrupt state.
     */
    uint64_t flags = arch_irq_save_disable();

    for (;;) {

        uint32_t expected = 0;

        /*
         * Atomic acquisition with acquire ordering.
         */
        if (__atomic_compare_exchange_n(
                &lock->value,
                &expected,
                1,
                false,
                __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED
            )) {

            *flags_out = flags;

            return true;
        }

        /*
         * Wait without continuously performing expensive
         * atomic compare-and-exchange operations.
         */
        while (__atomic_load_n(
                   &lock->value,
                   __ATOMIC_RELAXED
               ) != 0) {

            raw_spinlock_cpu_relax();
        }
    }
}


/*
 * Release a raw spinlock.
 */
bool raw_spinlock_unlock_irqrestore(
    raw_spinlock_t *lock,
    uint64_t flags
) {

    if (!raw_spinlock_is_valid(lock)) {

        /*
         * Do not accidentally leave interrupts disabled
         * if the caller supplies an invalid lock.
         */
        arch_irq_restore(flags);

        return false;
    }

    uint32_t expected = 1;

    /*
     * Atomic release.
     *
     * Release ordering ensures critical-section writes
     * become visible before another CPU acquires the lock.
     */
    bool released = __atomic_compare_exchange_n(
        &lock->value,
        &expected,
        0,
        false,
        __ATOMIC_RELEASE,
        __ATOMIC_RELAXED
    );

    /*
     * Restore the exact interrupt state that existed
     * before this lock was acquired.
     */
    arch_irq_restore(flags);

    return released;
}


/*
 * Return whether a raw spinlock is currently held.
 */
bool raw_spinlock_is_locked(
    const raw_spinlock_t *lock
) {

    if (!raw_spinlock_is_valid(lock)) {
        return false;
    }

    return __atomic_load_n(
        &lock->value,
        __ATOMIC_ACQUIRE
    ) != 0;
}
