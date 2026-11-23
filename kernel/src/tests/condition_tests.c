#include "tests/condition_tests.h"

#include "sync/condition.h"
#include "sync/mutex.h"

#include <stdint.h>
#include <stddef.h>


bool condition_run_tests(void) {

    condition_t condition = {0};
    mutex_t mutex = {0};


    /*
     * Test 1: Initialize condition variable.
     */
    if (!condition_init(&condition)) {
        return false;
    }

    if (!condition_is_initialized(&condition)) {
        return false;
    }

    if (condition_waiter_count(&condition) != 0) {
        return false;
    }


    /*
     * Test 2: Reject reinitialization.
     */
    if (condition_init(&condition)) {
        return false;
    }


    /*
     * Test 3: Signal with no waiters.
     *
     * This must succeed as a no-op.
     */
    if (!condition_signal(&condition)) {
        return false;
    }

    if (condition_waiter_count(&condition) != 0) {
        return false;
    }


    /*
     * Test 4: Broadcast with no waiters.
     */
    if (condition_broadcast(&condition) != 0) {
        return false;
    }


    /*
     * Test 5: Initialize mutex.
     */
    if (!mutex_init(&mutex)) {
        return false;
    }


    /*
     * Test 6: Reject condition_wait() when
     * the calling thread does not own the mutex.
     */
    if (condition_wait(&condition, &mutex)) {
        return false;
    }

    if (condition_waiter_count(&condition) != 0) {
        return false;
    }


    /*
     * Test 7: Invalid arguments.
     */
    if (condition_init(NULL)) {
        return false;
    }

    if (condition_wait(NULL, &mutex)) {
        return false;
    }

    if (condition_wait(&condition, NULL)) {
        return false;
    }

    if (condition_signal(NULL)) {
        return false;
    }

    if (condition_broadcast(NULL) != 0) {
        return false;
    }

    if (condition_waiter_count(NULL) != 0) {
        return false;
    }

    if (condition_is_initialized(NULL)) {
        return false;
    }


    /*
     * Test 8: Reject operations on an uninitialized
     * condition variable.
     */
    condition_t invalid_condition = {0};

    if (condition_signal(&invalid_condition)) {
        return false;
    }

    if (condition_broadcast(&invalid_condition) != 0) {
        return false;
    }

    if (condition_waiter_count(&invalid_condition) != 0) {
        return false;
    }


    return true;
}
