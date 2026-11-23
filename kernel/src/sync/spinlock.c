#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "sync/spinlock.h"
#include "scheduler/thread.h"
#include "arch/x86_64/irq_flags.h"


/*
 * CPU pause instruction.
 *
 * This is a processor hint used while waiting for a lock.
 * It does not release the lock or yield the CPU.
 */
static inline void spinlock_cpu_relax(void) {

    asm volatile ("pause" ::: "memory");
}


/*
 * Internal lock validation.
 */
static bool spinlock_is_valid(const spinlock_t *lock) {

    return lock != NULL && lock->initialized;
}


/*
 * Internal atomic acquisition.
 *
 * Acquire memory ordering ensures that memory operations
 * inside the critical section happen after lock acquisition.
 */
static void spinlock_acquire_internal(spinlock_t *lock) {

    for (;;) {

        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(
                &lock->value,
                &expected,
                1,
                false,
                __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED)) {

            return;
        }

        /*
         * Avoid repeatedly issuing expensive atomic operations
         * while another execution context owns the lock.
         */
        while (__atomic_load_n(
                   &lock->value,
                   __ATOMIC_RELAXED) != 0) {

            spinlock_cpu_relax();
        }
    }
}


/*
 * Initialize a spinlock.
 */
bool spinlock_init(spinlock_t *lock) {

    if (lock == NULL) {
        return false;
    }

    __atomic_store_n(
        &lock->value,
        0,
        __ATOMIC_RELAXED
    );

    lock->initialized = true;

    return true;
}


/*
 * Check whether a spinlock is held.
 */
bool spinlock_is_locked(const spinlock_t *lock) {

    if (!spinlock_is_valid(lock)) {
        return false;
    }

    return __atomic_load_n(
        &lock->value,
        __ATOMIC_ACQUIRE
    ) != 0;
}


/*
 * Acquire a spinlock.
 */
bool spinlock_lock(spinlock_t *lock) {

    if (!spinlock_is_valid(lock)) {
        return false;
    }

    /*
     * Prevent the scheduler from involuntarily switching
     * away from this thread while it owns or waits for
     * the spinlock.
     */
    if (!thread_preempt_disable()) {
        return false;
    }

    spinlock_acquire_internal(lock);

    return true;
}


/*
 * Release a spinlock.
 */
bool spinlock_unlock(spinlock_t *lock) {

    if (!spinlock_is_valid(lock)) {
        return false;
    }

    if (!spinlock_is_locked(lock)) {
        return false;
    }

    /*
     * Release ordering ensures critical-section writes
     * become visible before another CPU acquires the lock.
     */
    __atomic_store_n(
        &lock->value,
        0,
        __ATOMIC_RELEASE
    );

    /*
     * Restore one level of preemption protection.
     */
    if (!thread_preempt_enable()) {
        return false;
    }

    return true;
}


/*
 * Acquire a spinlock with local interrupts disabled.
 */
bool spinlock_lock_irqsave(
    spinlock_t *lock,
    uint64_t *flags_out
) {

    if (!spinlock_is_valid(lock) ||
        flags_out == NULL) {

        return false;
    }

    /*
     * Save the caller's original interrupt state and
     * disable local maskable interrupts.
     */
    uint64_t flags = arch_irq_save_disable();

    /*
     * Preemption protection is still necessary:
     * disabling interrupts alone is not a general
     * replacement for scheduler preemption control.
     */
    if (!thread_preempt_disable()) {

        arch_irq_restore(flags);

        return false;
    }

    spinlock_acquire_internal(lock);

    *flags_out = flags;

    return true;
}


/*
 * Release an IRQ-save spinlock and restore interrupt state.
 */
bool spinlock_unlock_irqrestore(
    spinlock_t *lock,
    uint64_t flags
) {

    if (!spinlock_is_valid(lock)) {
        return false;
    }

    if (!spinlock_is_locked(lock)) {
        return false;
    }

    __atomic_store_n(
        &lock->value,
        0,
        __ATOMIC_RELEASE
    );

    bool preemption_restored =
        thread_preempt_enable();

    /*
     * Restore exactly the interrupt state that existed
     * before spinlock_lock_irqsave().
     */
    arch_irq_restore(flags);

    return preemption_restored;
}
