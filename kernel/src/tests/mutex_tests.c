#include <stdbool.h>

#include "tests/mutex_tests.h"

#include "sync/mutex.h"


/* ============================================================
 * Initialization tests
 * ============================================================ */

static bool mutex_test_initialization(void) {

    mutex_t mutex = {0};

    if (mutex_is_locked(&mutex)) {
        return false;
    }

    if (!mutex_init(&mutex)) {
        return false;
    }

    if (mutex_is_locked(&mutex)) {
        return false;
    }

    /*
     * Reinitialization must be rejected.
     */
    if (mutex_init(&mutex)) {
        return false;
    }

    return true;
}


/* ============================================================
 * Basic acquisition tests
 * ============================================================ */

static bool mutex_test_acquisition(void) {

    mutex_t mutex = {0};

    if (!mutex_init(&mutex)) {
        return false;
    }

    /*
     * The first try-lock must succeed.
     */
    if (!mutex_try_lock(&mutex)) {
        return false;
    }

    if (!mutex_is_locked(&mutex)) {
        return false;
    }

    /*
     * Recursive acquisition must fail.
     */
    if (mutex_try_lock(&mutex)) {
        return false;
    }

    if (mutex_lock(&mutex)) {
        return false;
    }

    /*
     * The original owner must still hold the mutex.
     */
    if (!mutex_is_locked(&mutex)) {
        return false;
    }

    /*
     * Release must succeed.
     */
    if (!mutex_unlock(&mutex)) {
        return false;
    }

    if (mutex_is_locked(&mutex)) {
        return false;
    }

    /*
     * A second unlock must fail.
     */
    if (mutex_unlock(&mutex)) {
        return false;
    }

    return true;
}


/* ============================================================
 * Blocking API immediate-acquisition test
 * ============================================================ */

static bool mutex_test_lock_without_contention(void) {

    mutex_t mutex = {0};

    if (!mutex_init(&mutex)) {
        return false;
    }

    /*
     * With no existing owner or waiters, mutex_lock()
     * must acquire immediately without blocking.
     */
    if (!mutex_lock(&mutex)) {
        return false;
    }

    if (!mutex_is_locked(&mutex)) {
        return false;
    }

    if (!mutex_unlock(&mutex)) {
        return false;
    }

    return !mutex_is_locked(&mutex);
}


/* ============================================================
 * Invalid argument tests
 * ============================================================ */

static bool mutex_test_invalid_arguments(void) {

    if (mutex_init(NULL)) {
        return false;
    }

    if (mutex_lock(NULL)) {
        return false;
    }

    if (mutex_try_lock(NULL)) {
        return false;
    }

    if (mutex_unlock(NULL)) {
        return false;
    }

    if (mutex_is_locked(NULL)) {
        return false;
    }

    /*
     * Operations on an uninitialized mutex must fail.
     */
    mutex_t mutex = {0};

    if (mutex_lock(&mutex)) {
        return false;
    }

    if (mutex_try_lock(&mutex)) {
        return false;
    }

    if (mutex_unlock(&mutex)) {
        return false;
    }

    return true;
}


/* ============================================================
 * Test entry point
 * ============================================================ */

bool mutex_run_tests(void) {

    if (!mutex_test_initialization()) {
        return false;
    }

    if (!mutex_test_acquisition()) {
        return false;
    }

    if (!mutex_test_lock_without_contention()) {
        return false;
    }

    if (!mutex_test_invalid_arguments()) {
        return false;
    }

    return true;
}
