
#include "tests/condition_integration_tests.h"

#include "sync/condition.h"
#include "sync/mutex.h"

#include "scheduler/scheduler.h"
#include "scheduler/thread.h"
#include "scheduler/wait_queue.h"

#include <stdbool.h>
#include <stdint.h>


/*
 * Shared producer-consumer state.
 *
 * Static storage ensures these objects remain valid
 * while the consumer is blocked and later resumes.
 */
static condition_t condition_test_condition;
static mutex_t condition_test_mutex;

static volatile bool condition_test_ready;
static volatile bool condition_test_consumer_entered;
static volatile bool condition_test_consumer_saw_ready;
static volatile bool condition_test_consumer_finished;
static volatile bool condition_test_consumer_failed;


/*
 * Consumer thread.
 *
 * It waits until the producer changes condition_test_ready.
 *
 * The predicate is always checked while holding the mutex.
 */
static void condition_test_consumer(void *argument) {

    (void)argument;

    /*
     * Acquire the mutex before examining shared state.
     */
    if (!mutex_lock(&condition_test_mutex)) {

        condition_test_consumer_failed = true;
        return;
    }

    condition_test_consumer_entered = true;

    /*
     * A condition-variable wait can wake without the
     * predicate necessarily being true.
     *
     * Therefore, always recheck the predicate in a loop.
     */
    while (!condition_test_ready) {

        if (!condition_wait(
                &condition_test_condition,
                &condition_test_mutex
            )) {

            condition_test_consumer_failed = true;

            /*
             * condition_wait() normally returns with the
             * mutex reacquired. Release it if we own it.
             */
            if (condition_test_mutex.owner ==
                scheduler_get_current_thread(0)) {

                (void)mutex_unlock(&condition_test_mutex);
            }

            return;
        }
    }

    /*
     * condition_wait() must return with the mutex owned
     * by this consumer.
     */
    if (condition_test_mutex.owner !=
        scheduler_get_current_thread(0)) {

        condition_test_consumer_failed = true;
        return;
    }

    condition_test_consumer_saw_ready = true;

    /*
     * Release the mutex before terminating.
     */
    if (!mutex_unlock(&condition_test_mutex)) {

        condition_test_consumer_failed = true;
        return;
    }

    condition_test_consumer_finished = true;
}


/*
 * Test a complete condition-variable producer-consumer
 * interaction using actual scheduler context switches.
 *
 * Expected context switches:
 *
 * 1. Bootstrap -> consumer
 * 2. Consumer -> bootstrap (consumer blocks)
 * 3. Bootstrap -> consumer (consumer wakes and resumes)
 * 4. Consumer -> bootstrap (consumer exits)
 *
 * Expected voluntary yields:
 *
 * 1. Bootstrap yields to consumer.
 * 2. Bootstrap yields to resumed consumer.
 */
bool condition_integration_run_tests(void) {

    /*
     * The scheduler must already be operational.
     */
    if (!scheduler_is_initialized()) {
        return false;
    }

    struct scheduler_cpu *cpu =
        scheduler_get_cpu(0);

    if (cpu == NULL ||
        !cpu->initialized ||
        cpu->current_thread == NULL) {

        return false;
    }

    struct thread *bootstrap =
        cpu->current_thread;

    if (bootstrap->state != THREAD_STATE_RUNNING ||
        bootstrap->scheduling_class == THREAD_CLASS_IDLE ||
        bootstrap->preempt_count != 0) {

        return false;
    }

    /*
     * Start with clean scheduler queues.
     */
    if (cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    /*
     * Initialize the shared synchronization objects.
     *
     * Both objects have static zero-initialized storage.
     */
    if (!mutex_init(&condition_test_mutex)) {
        return false;
    }

    if (!condition_init(&condition_test_condition)) {
        return false;
    }

    /*
     * Initialize shared state.
     */
    condition_test_ready = false;
    condition_test_consumer_entered = false;
    condition_test_consumer_saw_ready = false;
    condition_test_consumer_finished = false;
    condition_test_consumer_failed = false;

    /*
     * Record scheduler diagnostics.
     */
    uint64_t initial_context_switches =
        cpu->context_switch_count;

    uint64_t initial_voluntary_yields =
        cpu->voluntary_yield_count;

    if (initial_context_switches > UINT64_MAX - 4 ||
        initial_voluntary_yields > UINT64_MAX - 2) {

        return false;
    }

    /*
     * Create the consumer.
     */
    struct thread *consumer = thread_create(
        "condition-consumer",
        condition_test_consumer,
        NULL,
        0
    );

    if (consumer == NULL) {
        return false;
    }

    /*
     * Make the consumer runnable.
     */
    if (!scheduler_enqueue_thread(0, consumer)) {

        (void)thread_destroy(consumer);
        return false;
    }

    /*
     * Let the consumer run.
     *
     * It should:
     * - Acquire the mutex.
     * - Observe that the predicate is false.
     * - Release the mutex through condition_wait().
     * - Block on the condition wait queue.
     *
     * The bootstrap thread resumes when the consumer blocks.
     */
    if (!thread_yield()) {
        return false;
    }

    /*
     * Verify that the consumer reached condition_wait().
     */
    if (!condition_test_consumer_entered ||
        condition_test_consumer_failed ||
        condition_test_consumer_saw_ready ||
        condition_test_consumer_finished) {

        return false;
    }

    /*
     * Verify the actual BLOCKED state.
     */
    if (consumer->state != THREAD_STATE_BLOCKED ||
        consumer->wait_channel !=
            &condition_test_condition.waiters ||
        condition_waiter_count(
            &condition_test_condition
        ) != 1) {

        return false;
    }

    if (!wait_queue_validate(
            &condition_test_condition.waiters
        )) {

        return false;
    }

    /*
     * Critical condition-variable property:
     *
     * The consumer must have released the mutex before
     * entering the condition wait queue.
     */
    if (mutex_is_locked(&condition_test_mutex) ||
        condition_test_mutex.owner != NULL) {

        return false;
    }

    /*
     * The bootstrap thread now acts as the producer.
     *
     * Acquire the same mutex before changing the predicate.
     */
    if (!mutex_lock(&condition_test_mutex)) {
        return false;
    }

    /*
     * Publish the condition while holding the mutex.
     */
    condition_test_ready = true;

    /*
     * Notify one condition-variable waiter.
     */
    bool signal_result =
        condition_signal(&condition_test_condition);

    /*
     * Release the producer's mutex ownership.
     *
     * Do this even if signaling reported failure, so
     * the test does not unnecessarily leave the mutex held.
     */
    bool unlock_result =
        mutex_unlock(&condition_test_mutex);

    if (!signal_result || !unlock_result) {
        return false;
    }

    /*
     * The consumer should now be READY.
     *
     * It has been removed from the condition wait queue,
     * but it has not executed again yet.
     */
    if (!wait_queue_is_empty(
            &condition_test_condition.waiters
        ) ||
        !wait_queue_validate(
            &condition_test_condition.waiters
        )) {

        return false;
    }

    if (consumer->state != THREAD_STATE_READY ||
        consumer->wait_channel != NULL) {

        return false;
    }

    /*
     * Resume the consumer.
     *
     * It should:
     * - Return from condition_wait().
     * - Reacquire the mutex.
     * - Observe condition_test_ready == true.
     * - Release the mutex.
     * - Return from its entry function.
     * - Be terminated and reaped.
     */
    if (!thread_yield()) {
        return false;
    }

    /*
     * The consumer may already have been destroyed.
     *
     * Do not dereference the consumer pointer here.
     * Inspect only independent state and scheduler data.
     */
    if (condition_test_consumer_failed ||
        !condition_test_consumer_saw_ready ||
        !condition_test_consumer_finished) {

        return false;
    }

    /*
     * The bootstrap thread must be running again.
     */
    if (cpu->current_thread != bootstrap ||
        bootstrap->state != THREAD_STATE_RUNNING) {

        return false;
    }

    /*
     * The consumer must have released the mutex.
     */
    if (mutex_is_locked(&condition_test_mutex) ||
        condition_test_mutex.owner != NULL) {

        return false;
    }

    /*
     * The condition wait queue must be empty and valid.
     */
    if (condition_waiter_count(
            &condition_test_condition
        ) != 0 ||
        !wait_queue_is_empty(
            &condition_test_condition.waiters
        ) ||
        !wait_queue_validate(
            &condition_test_condition.waiters
        )) {

        return false;
    }

    /*
     * No runnable or terminated worker should remain.
     */
    if (cpu->run_queue.runnable_count != 0 ||
        cpu->reap_count != 0 ||
        cpu->reap_head != NULL ||
        cpu->reap_tail != NULL) {

        return false;
    }

    /*
     * Verify the exact expected context-switch count.
     */
    if (cpu->context_switch_count !=
        initial_context_switches + 4) {

        return false;
    }

    /*
     * Verify the two expected bootstrap yields.
     */
    if (cpu->voluntary_yield_count !=
        initial_voluntary_yields + 2) {

        return false;
    }

    return true;
}
