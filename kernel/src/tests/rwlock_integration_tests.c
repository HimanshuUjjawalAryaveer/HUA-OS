#include "tests/rwlock_integration_tests.h"

#include <stdbool.h>

#include "sync/rwlock.h"
#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "scheduler/wait_queue.h"


/*
 * Shared integration-test state.
 */
static rwlock_t test_lock;

static struct wait_queue reader_hold_queue;
static struct wait_queue writer_hold_queue;


/*
 * Worker progress flags.
 */
static volatile bool test_error;

static volatile bool simultaneous_reader_acquired;
static volatile bool simultaneous_reader_finished;

static volatile bool waiting_writer_acquired;
static volatile bool waiting_writer_finished;

static volatile bool waiting_reader_acquired;
static volatile bool waiting_reader_finished;


/*
 * Worker 1:
 *
 * Acquires a read lock, then blocks while retaining it.
 * The bootstrap thread wakes it later.
 */
static void simultaneous_reader_worker(void *argument) {

    (void)argument;

    if (!rwlock_read_lock(&test_lock)) {
        test_error = true;
        return;
    }

    simultaneous_reader_acquired = true;

    /*
     * Retain the read lock while blocked.
     */
    if (!thread_block(&reader_hold_queue)) {
        test_error = true;
    }

    if (!rwlock_read_unlock(&test_lock)) {
        test_error = true;
        return;
    }

    simultaneous_reader_finished = true;
}


/*
 * Worker 2:
 *
 * Attempts to acquire the write lock.
 *
 * Once acquired, it blocks while retaining exclusive
 * ownership. The bootstrap thread later wakes it.
 */
static void waiting_writer_worker(void *argument) {

    (void)argument;

    if (!rwlock_write_lock(&test_lock)) {
        test_error = true;
        return;
    }

    waiting_writer_acquired = true;

    if (!thread_block(&writer_hold_queue)) {
        test_error = true;
    }

    if (!rwlock_write_unlock(&test_lock)) {
        test_error = true;
        return;
    }

    waiting_writer_finished = true;
}


/*
 * Worker 3:
 *
 * Attempts to acquire a read lock after a writer
 * has already started waiting.
 */
static void waiting_reader_worker(void *argument) {

    (void)argument;

    if (!rwlock_read_lock(&test_lock)) {
        test_error = true;
        return;
    }

    waiting_reader_acquired = true;

    if (!rwlock_read_unlock(&test_lock)) {
        test_error = true;
        return;
    }

    waiting_reader_finished = true;
}


/*
 * Create and enqueue a worker thread.
 */
static bool rwlock_test_start_worker(
    const char *name,
    thread_entry_t entry
) {

    struct thread *worker =
        thread_create(name, entry, NULL, 0);

    if (worker == NULL) {
        return false;
    }

    if (!scheduler_enqueue_thread(0, worker)) {
        return false;
    }

    return true;
}


/*
 * Phase 1:
 *
 * Verify that multiple readers can hold the same
 * RW lock simultaneously.
 */
static bool rwlock_test_simultaneous_readers(void) {

    simultaneous_reader_acquired = false;
    simultaneous_reader_finished = false;

    test_error = false;

    if (!rwlock_read_lock(&test_lock)) {
        return false;
    }

    if (!rwlock_test_start_worker(
            "rw-reader",
            simultaneous_reader_worker
        )) {

        rwlock_read_unlock(&test_lock);
        return false;
    }

    /*
     * Let the worker acquire its read lock and block.
     */
    if (!thread_yield()) {
        rwlock_read_unlock(&test_lock);
        return false;
    }

    /*
     * Both bootstrap and worker must hold read locks.
     */
    if (test_error ||
        !simultaneous_reader_acquired ||
        simultaneous_reader_finished ||
        rwlock_reader_count(&test_lock) != 2 ||
        rwlock_writer_is_active(&test_lock) ||
        wait_queue_count(&reader_hold_queue) != 1) {

        rwlock_read_unlock(&test_lock);
        return false;
    }

    /*
     * Release the bootstrap thread's read lock.
     */
    if (!rwlock_read_unlock(&test_lock)) {
        return false;
    }

    if (rwlock_reader_count(&test_lock) != 1) {
        return false;
    }

    /*
     * Wake the worker that is retaining the other
     * read lock.
     */
    if (!wait_queue_wake_one(&reader_hold_queue)) {
        return false;
    }

    /*
     * Give the worker an opportunity to finish.
     */
    for (uint64_t i = 0;
         i < 8 && !simultaneous_reader_finished;
         i++) {

        if (!thread_yield()) {
            return false;
        }
    }

    if (test_error ||
        !simultaneous_reader_finished ||
        rwlock_reader_count(&test_lock) != 0 ||
        rwlock_writer_is_active(&test_lock) ||
        !wait_queue_is_empty(&reader_hold_queue)) {

        return false;
    }

    return true;
}


/*
 * Phase 2:
 *
 * Verify writer preference.
 *
 * Sequence:
 *
 * 1. Bootstrap acquires a read lock.
 * 2. Writer attempts write lock and blocks.
 * 3. Another reader attempts read lock and blocks.
 * 4. Bootstrap releases its read lock.
 * 5. Writer receives ownership.
 * 6. Writer remains active while reader stays blocked.
 * 7. Writer releases ownership.
 * 8. Waiting reader acquires and releases its lock.
 */
static bool rwlock_test_writer_preference(void) {

    waiting_writer_acquired = false;
    waiting_writer_finished = false;

    waiting_reader_acquired = false;
    waiting_reader_finished = false;

    test_error = false;

    /*
     * Bootstrap becomes the first reader.
     */
    if (!rwlock_read_lock(&test_lock)) {
        return false;
    }

    /*
     * Start the writer.
     */
    if (!rwlock_test_start_worker(
            "rw-writer",
            waiting_writer_worker
        )) {

        rwlock_read_unlock(&test_lock);
        return false;
    }

    /*
     * Writer runs and blocks because bootstrap
     * still holds a read lock.
     */
    if (!thread_yield()) {
        rwlock_read_unlock(&test_lock);
        return false;
    }

    if (test_error ||
        waiting_writer_acquired ||
        test_lock.waiting_writers != 1 ||
        wait_queue_count(&test_lock.writer_waiters) != 1 ||
        rwlock_reader_count(&test_lock) != 1) {

        rwlock_read_unlock(&test_lock);
        return false;
    }

    /*
     * Start another reader AFTER the writer is waiting.
     */
    if (!rwlock_test_start_worker(
            "rw-late-reader",
            waiting_reader_worker
        )) {

        rwlock_read_unlock(&test_lock);
        return false;
    }

    /*
     * The late reader must block instead of bypassing
     * the waiting writer.
     */
    if (!thread_yield()) {
        rwlock_read_unlock(&test_lock);
        return false;
    }

    if (test_error ||
        waiting_reader_acquired ||
        wait_queue_count(&test_lock.reader_waiters) != 1 ||
        test_lock.waiting_writers != 1) {

        rwlock_read_unlock(&test_lock);
        return false;
    }

    /*
     * Release the final active reader.
     *
     * The RW lock should hand ownership to the writer.
     */
    if (!rwlock_read_unlock(&test_lock)) {
        return false;
    }

    if (!rwlock_writer_is_active(&test_lock)) {
        return false;
    }

    /*
     * Run the writer.
     *
     * It should resume with ownership already transferred,
     * then block on writer_hold_queue.
     */
    if (!thread_yield()) {
        return false;
    }

    if (test_error ||
        !waiting_writer_acquired ||
        waiting_writer_finished ||
        !rwlock_writer_is_active(&test_lock) ||
        rwlock_reader_count(&test_lock) != 0 ||
        waiting_reader_acquired ||
        wait_queue_count(&test_lock.reader_waiters) != 1 ||
        wait_queue_count(&writer_hold_queue) != 1) {

        return false;
    }

    /*
     * Release the writer.
     *
     * With no waiting writers remaining, the waiting
     * reader should now be awakened.
     */
    if (!wait_queue_wake_one(&writer_hold_queue)) {
        return false;
    }

    /*
     * Run the writer and then the waiting reader.
     */
    for (uint64_t i = 0;
         i < 8 &&
         (!waiting_writer_finished ||
          !waiting_reader_finished);
         i++) {

        if (!thread_yield()) {
            return false;
        }
    }

    if (test_error ||
        !waiting_writer_finished ||
        !waiting_reader_acquired ||
        !waiting_reader_finished ||
        rwlock_writer_is_active(&test_lock) ||
        rwlock_reader_count(&test_lock) != 0 ||
        !wait_queue_is_empty(&test_lock.reader_waiters) ||
        !wait_queue_is_empty(&test_lock.writer_waiters) ||
        !wait_queue_is_empty(&writer_hold_queue)) {

        return false;
    }

    return true;
}


/*
 * Public integration-test entry point.
 */
bool rwlock_integration_run_tests(void) {

    test_error = false;

    /*
     * Initialize the lock and test wait queues.
     */
    if (!rwlock_init(&test_lock)) {
        return false;
    }

    if (!wait_queue_init(&reader_hold_queue)) {
        return false;
    }

    if (!wait_queue_init(&writer_hold_queue)) {
        return false;
    }

    /*
     * Test simultaneous readers.
     */
    if (!rwlock_test_simultaneous_readers()) {
        return false;
    }

    /*
     * Test writer preference and handoff.
     */
    if (!rwlock_test_writer_preference()) {
        return false;
    }

    /*
     * Final consistency checks.
     */
    if (rwlock_reader_count(&test_lock) != 0 ||
        rwlock_writer_is_active(&test_lock) ||
        test_lock.waiting_writers != 0 ||
        !wait_queue_is_empty(&test_lock.reader_waiters) ||
        !wait_queue_is_empty(&test_lock.writer_waiters) ||
        !wait_queue_is_empty(&reader_hold_queue) ||
        !wait_queue_is_empty(&writer_hold_queue)) {

        return false;
    }

    return true;
}
