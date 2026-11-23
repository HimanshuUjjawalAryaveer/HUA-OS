#include "tests/rwlock_tests.h"

#include <stdint.h>

#include "sync/rwlock.h"


bool rwlock_run_tests(void) {

    rwlock_t lock = {0};

    /*
     * Test 1:
     * Initialization.
     */
    if (!rwlock_init(&lock)) {
        return false;
    }

    if (!rwlock_is_initialized(&lock)) {
        return false;
    }

    if (rwlock_reader_count(&lock) != 0) {
        return false;
    }

    if (rwlock_writer_is_active(&lock)) {
        return false;
    }

    /*
     * Reinitialization must be rejected.
     */
    if (rwlock_init(&lock)) {
        return false;
    }

    /*
     * Test 2:
     * Acquire a read lock.
     */
    if (!rwlock_read_lock(&lock)) {
        return false;
    }

    if (rwlock_reader_count(&lock) != 1) {
        return false;
    }

    if (rwlock_writer_is_active(&lock)) {
        return false;
    }

    /*
     * Test 3:
     * Recursive read acquisition must be rejected.
     */
    if (rwlock_try_read_lock(&lock)) {
        return false;
    }

    if (rwlock_reader_count(&lock) != 1) {
        return false;
    }

    /*
     * Test 4:
     * A reader cannot upgrade itself to a writer.
     */
    if (rwlock_try_write_lock(&lock)) {
        return false;
    }

    if (rwlock_writer_is_active(&lock)) {
        return false;
    }

    /*
     * Test 5:
     * A reader cannot release a writer lock.
     */
    if (rwlock_write_unlock(&lock)) {
        return false;
    }

    /*
     * Test 6:
     * Release the read lock.
     */
    if (!rwlock_read_unlock(&lock)) {
        return false;
    }

    if (rwlock_reader_count(&lock) != 0) {
        return false;
    }

    /*
     * Releasing a read lock twice must fail.
     */
    if (rwlock_read_unlock(&lock)) {
        return false;
    }

    /*
     * Test 7:
     * Acquire a writer lock.
     */
    if (!rwlock_write_lock(&lock)) {
        return false;
    }

    if (!rwlock_writer_is_active(&lock)) {
        return false;
    }

    if (rwlock_reader_count(&lock) != 0) {
        return false;
    }

    /*
     * Test 8:
     * Readers must not enter while a writer owns the lock.
     */
    if (rwlock_try_read_lock(&lock)) {
        return false;
    }

    /*
     * Test 9:
     * A second writer acquisition by the owner must fail.
     */
    if (rwlock_try_write_lock(&lock)) {
        return false;
    }

    /*
     * Test 10:
     * A writer cannot release a read lock.
     */
    if (rwlock_read_unlock(&lock)) {
        return false;
    }

    /*
     * Test 11:
     * Release the writer lock.
     */
    if (!rwlock_write_unlock(&lock)) {
        return false;
    }

    if (rwlock_writer_is_active(&lock)) {
        return false;
    }

    /*
     * Releasing the writer lock twice must fail.
     */
    if (rwlock_write_unlock(&lock)) {
        return false;
    }

    /*
     * Test 12:
     * Try-read should work again after writer release.
     */
    if (!rwlock_try_read_lock(&lock)) {
        return false;
    }

    if (rwlock_reader_count(&lock) != 1) {
        return false;
    }

    if (!rwlock_read_unlock(&lock)) {
        return false;
    }

    /*
     * Test 13:
     * Try-write should work after all readers leave.
     */
    if (!rwlock_try_write_lock(&lock)) {
        return false;
    }

    if (!rwlock_writer_is_active(&lock)) {
        return false;
    }

    if (!rwlock_write_unlock(&lock)) {
        return false;
    }

    /*
     * Final state must be completely unlocked.
     */
    if (rwlock_reader_count(&lock) != 0 ||
        rwlock_writer_is_active(&lock)) {

        return false;
    }

    return true;
}
