#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "drivers/console/console.h"
#include "sync/spinlock.h"
#include "spinlock_tests.h"


#define SPINLOCK_TEST_IF_FLAG (1ULL << 9)


static bool spinlock_test_failure(const char *message) {

    console_set_color(0xFF5555, 0x101820);

    console_write("[ FAILED ] ");
    console_write(message);
    console_write("\n");

    console_set_color(0xFFFFFF, 0x101820);

    return false;
}


static void spinlock_test_success(const char *message) {

    console_set_color(0x36D399, 0x101820);

    console_write("[ OK ] ");
    console_write(message);
    console_write("\n");

    console_set_color(0xFFFFFF, 0x101820);
}


/*
 * Read the current RFLAGS register.
 */
static uint64_t spinlock_test_read_rflags(void) {

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


bool spinlock_run_tests(void) {

    console_write("\n");
    console_write("===== SPINLOCK TESTS =====\n");


    spinlock_t first = {0};
    spinlock_t second = {0};
    spinlock_t invalid = {0};


    /*
     * TEST 1:
     * Reject invalid arguments.
     */
    if (spinlock_init(NULL) ||
        spinlock_lock(NULL) ||
        spinlock_unlock(NULL) ||
        spinlock_lock_irqsave(NULL, NULL) ||
        spinlock_unlock_irqrestore(NULL, 0)) {

        return spinlock_test_failure(
            "Spinlock invalid argument rejection"
        );
    }

    spinlock_test_success(
        "Spinlock invalid argument rejection"
    );


    /*
     * TEST 2:
     * Initialize spinlocks.
     */
    if (!spinlock_init(&first) ||
        !spinlock_init(&second)) {

        return spinlock_test_failure(
            "Spinlock initialization"
        );
    }

    if (spinlock_is_locked(&first) ||
        spinlock_is_locked(&second)) {

        return spinlock_test_failure(
            "New spinlocks start unlocked"
        );
    }

    spinlock_test_success(
        "Spinlock initialization and unlocked state"
    );


    /*
     * TEST 3:
     * Reject operations on an uninitialized lock.
     */
    if (spinlock_lock(&invalid) ||
        spinlock_unlock(&invalid) ||
        spinlock_is_locked(&invalid)) {

        return spinlock_test_failure(
            "Uninitialized spinlock rejection"
        );
    }

    spinlock_test_success(
        "Uninitialized spinlock rejection"
    );


    /*
     * TEST 4:
     * Acquire and release a spinlock.
     */
    if (!spinlock_lock(&first)) {

        return spinlock_test_failure(
            "Spinlock acquisition"
        );
    }

    if (!spinlock_is_locked(&first)) {

        spinlock_unlock(&first);

        return spinlock_test_failure(
            "Spinlock locked state"
        );
    }

    if (!spinlock_unlock(&first)) {

        return spinlock_test_failure(
            "Spinlock release"
        );
    }

    if (spinlock_is_locked(&first)) {

        return spinlock_test_failure(
            "Spinlock unlocked state"
        );
    }

    spinlock_test_success(
        "Spinlock acquisition and release"
    );


    /*
     * TEST 5:
     * Reject releasing an unlocked spinlock.
     */
    if (spinlock_unlock(&first)) {

        return spinlock_test_failure(
            "Unlocked spinlock release rejection"
        );
    }

    spinlock_test_success(
        "Unlocked spinlock release rejection"
    );


    /*
     * TEST 6:
     * Nested preemption protection using two locks.
     *
     * Both locks are distinct. This tests that acquiring
     * a second lock does not prematurely re-enable
     * preemption when the first lock is still held.
     */
    if (!spinlock_lock(&first)) {

        return spinlock_test_failure(
            "Nested spinlock first acquisition"
        );
    }

    if (!spinlock_lock(&second)) {

        spinlock_unlock(&first);

        return spinlock_test_failure(
            "Nested spinlock second acquisition"
        );
    }

    if (!spinlock_is_locked(&first) ||
        !spinlock_is_locked(&second)) {

        spinlock_unlock(&second);
        spinlock_unlock(&first);

        return spinlock_test_failure(
            "Nested spinlock locked state"
        );
    }

    if (!spinlock_unlock(&second)) {

        spinlock_unlock(&first);

        return spinlock_test_failure(
            "Nested spinlock second release"
        );
    }

    if (!spinlock_is_locked(&first) ||
        spinlock_is_locked(&second)) {

        spinlock_unlock(&first);

        return spinlock_test_failure(
            "Nested spinlock preemption nesting"
        );
    }

    if (!spinlock_unlock(&first)) {

        return spinlock_test_failure(
            "Nested spinlock first release"
        );
    }

    spinlock_test_success(
        "Nested spinlock and preemption protection"
    );


    /*
     * TEST 7:
     * IRQ-save lock acquisition and interrupt-state
     * preservation.
     */
    uint64_t flags_before =
        spinlock_test_read_rflags();

    uint64_t saved_flags = 0;

    if (!spinlock_lock_irqsave(
            &first,
            &saved_flags)) {

        return spinlock_test_failure(
            "IRQ-save spinlock acquisition"
        );
    }

    uint64_t flags_during =
        spinlock_test_read_rflags();

    if ((flags_during & SPINLOCK_TEST_IF_FLAG) != 0 ||
        !spinlock_is_locked(&first)) {

        spinlock_unlock_irqrestore(
            &first,
            saved_flags
        );

        return spinlock_test_failure(
            "IRQ-save spinlock disables interrupts"
        );
    }

    if (!spinlock_unlock_irqrestore(
            &first,
            saved_flags)) {

        return spinlock_test_failure(
            "IRQ-save spinlock release"
        );
    }

    uint64_t flags_after =
        spinlock_test_read_rflags();

    if ((flags_before & SPINLOCK_TEST_IF_FLAG) !=
        (flags_after & SPINLOCK_TEST_IF_FLAG)) {

        return spinlock_test_failure(
            "IRQ-save interrupt-state restoration"
        );
    }

    spinlock_test_success(
        "IRQ-save spinlock and interrupt-state preservation"
    );


    /*
     * TEST 8:
     * Confirm the IRQ-save lock is no longer held.
     */
    if (spinlock_is_locked(&first)) {

        return spinlock_test_failure(
            "IRQ-save lock final unlocked state"
        );
    }

    spinlock_test_success(
        "IRQ-save lock final unlocked state"
    );


    console_write("===== SPINLOCK TESTS COMPLETE =====\n\n");

    return true;
}
