
#include "tests/raw_spinlock_tests.h"

#include "sync/raw_spinlock.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


#define RAW_SPINLOCK_TEST_IF_FLAG (1ULL << 9)


/*
 * Read the current RFLAGS register.
 */
static uint64_t raw_spinlock_test_read_rflags(void) {

    uint64_t flags;

    asm volatile (
        "pushfq\n\t"
        "popq %0"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}


/*
 * Check whether maskable interrupts are currently enabled.
 */
static bool raw_spinlock_test_interrupts_enabled(void) {

    return (
        raw_spinlock_test_read_rflags() &
        RAW_SPINLOCK_TEST_IF_FLAG
    ) != 0;
}


/*
 * Test initialization, interrupt handling and lock lifecycle.
 */
bool raw_spinlock_run_tests(void) {

    raw_spinlock_t first_lock = {0};
    raw_spinlock_t second_lock = {0};

    /*
     * Invalid argument tests.
     */
    if (raw_spinlock_init(NULL)) {
        return false;
    }

    if (raw_spinlock_is_locked(NULL)) {
        return false;
    }

    /*
     * Initialize two independent locks.
     */
    if (!raw_spinlock_init(&first_lock) ||
        !raw_spinlock_init(&second_lock)) {

        return false;
    }

    /*
     * Double initialization must fail.
     */
    if (raw_spinlock_init(&first_lock)) {
        return false;
    }

    if (raw_spinlock_is_locked(&first_lock) ||
        raw_spinlock_is_locked(&second_lock)) {

        return false;
    }

    /*
     * A missing flags output pointer must be rejected
     * without changing the interrupt state.
     */
    uint64_t original_rflags =
        raw_spinlock_test_read_rflags();

    if (raw_spinlock_lock_irqsave(
            &first_lock,
            NULL
        )) {

        return false;
    }

    uint64_t current_rflags =
        raw_spinlock_test_read_rflags();

    if ((original_rflags ^ current_rflags) &
        RAW_SPINLOCK_TEST_IF_FLAG) {

        return false;
    }

    /*
     * Remember the interrupt state before acquisition.
     */
    bool original_interrupt_state =
        raw_spinlock_test_interrupts_enabled();

    uint64_t first_flags = 0;
    uint64_t second_flags = 0;

    bool first_held = false;
    bool second_held = false;

    /*
     * Acquire the first lock.
     */
    if (!raw_spinlock_lock_irqsave(
            &first_lock,
            &first_flags
        )) {

        goto failure;
    }

    first_held = true;

    if (!raw_spinlock_is_locked(&first_lock)) {
        goto failure;
    }

    /*
     * Interrupts must be disabled while the lock is held.
     */
    if (raw_spinlock_test_interrupts_enabled()) {
        goto failure;
    }

    /*
     * Acquire a second independent lock.
     *
     * Its saved interrupt state should also be disabled,
     * because the first lock is still held.
     */
    if (!raw_spinlock_lock_irqsave(
            &second_lock,
            &second_flags
        )) {

        goto failure;
    }

    second_held = true;

    if (!raw_spinlock_is_locked(&second_lock)) {
        goto failure;
    }

    if (raw_spinlock_test_interrupts_enabled()) {
        goto failure;
    }

    /*
     * Release the second lock.
     *
     * Interrupts must remain disabled because the first
     * lock is still held.
     */
    if (!raw_spinlock_unlock_irqrestore(
            &second_lock,
            second_flags
        )) {

        second_held = false;
        goto failure;
    }

    second_held = false;

    if (raw_spinlock_is_locked(&second_lock)) {
        goto failure;
    }

    if (raw_spinlock_test_interrupts_enabled()) {
        goto failure;
    }

    /*
     * Release the first lock.
     *
     * The original interrupt state must be restored.
     */
    if (!raw_spinlock_unlock_irqrestore(
            &first_lock,
            first_flags
        )) {

        first_held = false;
        goto failure;
    }

    first_held = false;

    if (raw_spinlock_is_locked(&first_lock)) {
        goto failure;
    }

    if (raw_spinlock_is_locked(&second_lock)) {
        goto failure;
    }

    if (raw_spinlock_test_interrupts_enabled() !=
        original_interrupt_state) {

        return false;
    }

    return true;


failure:

    /*
     * Release any locks still held by this test.
     */
    if (second_held) {

        (void)raw_spinlock_unlock_irqrestore(
            &second_lock,
            second_flags
        );
    }

    if (first_held) {

        (void)raw_spinlock_unlock_irqrestore(
            &first_lock,
            first_flags
        );
    }

    return false;
}
