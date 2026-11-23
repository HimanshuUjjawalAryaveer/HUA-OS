#include "tests/semaphore_tests.h"

#include "sync/semaphore.h"

#include <stdint.h>
#include <stddef.h>
#include <limits.h>


bool semaphore_run_tests(void) {

    semaphore_t semaphore = {0};


    /*
     * Test 1: Initialization.
     */
    if (!semaphore_init(&semaphore, 3)) {
        return false;
    }

    if (!semaphore_is_initialized(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 3) {
        return false;
    }


    /*
     * Test 2: Reject reinitialization.
     */
    if (semaphore_init(&semaphore, 5)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 3) {
        return false;
    }


    /*
     * Test 3: Acquire permits without blocking.
     */
    if (!semaphore_try_down(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 2) {
        return false;
    }

    if (!semaphore_down(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 1) {
        return false;
    }

    if (!semaphore_try_down(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 0) {
        return false;
    }


    /*
     * Test 4: Acquisition must fail when
     * no permits are available.
     *
     * try_down() must not block.
     */
    if (semaphore_try_down(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 0) {
        return false;
    }


    /*
     * Test 5: Release permits.
     */
    if (!semaphore_up(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 1) {
        return false;
    }

    if (!semaphore_up(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 2) {
        return false;
    }


    /*
     * Test 6: Acquire all released permits.
     */
    if (!semaphore_down(&semaphore)) {
        return false;
    }

    if (!semaphore_down(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != 0) {
        return false;
    }


    /*
     * Test 7: Counter overflow protection.
     *
     * Directly set the counter to its maximum
     * value to test the overflow boundary.
     */
    semaphore.count = UINT64_MAX;

    if (semaphore_up(&semaphore)) {
        return false;
    }

    if (semaphore_get_count(&semaphore) != UINT64_MAX) {
        return false;
    }


    /*
     * Test 8: Invalid arguments.
     */
    if (semaphore_init(NULL, 1)) {
        return false;
    }

    if (semaphore_down(NULL)) {
        return false;
    }

    if (semaphore_try_down(NULL)) {
        return false;
    }

    if (semaphore_up(NULL)) {
        return false;
    }

    if (semaphore_get_count(NULL) != 0) {
        return false;
    }

    if (semaphore_is_initialized(NULL)) {
        return false;
    }


    /*
     * Test 9: Zero-initialized but uninitialized
     * semaphore must reject operations.
     */
    semaphore_t invalid_semaphore = {0};

    if (semaphore_down(&invalid_semaphore)) {
        return false;
    }

    if (semaphore_try_down(&invalid_semaphore)) {
        return false;
    }

    if (semaphore_up(&invalid_semaphore)) {
        return false;
    }

    if (semaphore_get_count(&invalid_semaphore) != 0) {
        return false;
    }


    return true;
}
