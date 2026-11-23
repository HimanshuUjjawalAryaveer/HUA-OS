#include "tests/thread_tests.h"

#include "scheduler/thread.h"

#include "arch/x86_64/thread_bootstrap.h"
#include "mm/pmm.h"


/*
 * Test entry function.
 *
 * It is never executed by these tests.
 */
static void thread_test_entry(void *argument) {
    (void)argument;
}


/*
 * Compare two strings without relying on libc.
 */
static bool thread_test_string_equals(
    const char *left,
    const char *right
) {

    if (left == NULL || right == NULL) {
        return false;
    }

    size_t i = 0;

    while (left[i] != '\0' && right[i] != '\0') {

        if (left[i] != right[i]) {
            return false;
        }

        i++;
    }

    return left[i] == right[i];
}


/*
 * Verify thread creation, stack construction, and cleanup.
 */
static bool thread_test_creation(void) {

    struct thread *thread = thread_create(
        "thread-test",
        thread_test_entry,
        NULL,
        0
    );

    if (thread == NULL) {
        return false;
    }

    bool valid =
        thread->id != 0 &&
        thread->state == THREAD_STATE_NEW &&
        thread->scheduling_class == THREAD_CLASS_FAIR &&
        thread_test_string_equals(
            thread->name,
            "thread-test"
        ) &&
        thread->entry == thread_test_entry &&
        thread->entry_argument == NULL &&
        thread->stack_owned &&
        thread->stack_base != NULL &&
        thread->stack_size == THREAD_STACK_DEFAULT_SIZE &&
        thread->stack_top ==
            (uintptr_t)thread->stack_base + thread->stack_size &&
        (((uintptr_t)thread & 15ULL) == 0) &&
        (((uintptr_t)thread->stack_base &
          (PMM_PAGE_SIZE - 1ULL)) == 0) &&
        ((thread->stack_top & 15ULL) == 0) &&
        thread->affinity_mask == 1ULL &&
        thread->fair_weight == 1024ULL &&
        thread->context.r12 == (uint64_t)(uintptr_t)thread &&
        ((thread->context.rsp & 15ULL) == 0);

    if (valid) {

        uintptr_t *initial_stack =
            (uintptr_t *)(uintptr_t)thread->context.rsp;

        valid =
            initial_stack[0] ==
                (uintptr_t)arch_thread_bootstrap &&
            initial_stack[1] ==
                (uintptr_t)arch_thread_return_trap;
    }

    if (!thread_destroy(thread)) {
        return false;
    }

    return valid;
}


/*
 * Verify custom stack sizing and page rounding.
 */
static bool thread_test_custom_stack(void) {

    size_t requested_size =
        THREAD_STACK_DEFAULT_SIZE + 1ULL;

    struct thread *thread = thread_create(
        "custom-stack",
        thread_test_entry,
        NULL,
        requested_size
    );

    if (thread == NULL) {
        return false;
    }

    size_t expected_size =
        THREAD_STACK_DEFAULT_SIZE + PMM_PAGE_SIZE;

    bool valid =
        thread->stack_size == expected_size &&
        thread->stack_top -
            (uintptr_t)thread->stack_base == expected_size;

    if (!thread_destroy(thread)) {
        return false;
    }

    return valid;
}


/*
 * Verify null names receive a valid fallback name.
 */
static bool thread_test_default_name(void) {

    struct thread *thread = thread_create(
        NULL,
        thread_test_entry,
        NULL,
        0
    );

    if (thread == NULL) {
        return false;
    }

    bool valid =
        thread_test_string_equals(
            thread->name,
            "unnamed"
        );

    if (!thread_destroy(thread)) {
        return false;
    }

    return valid;
}


/*
 * Verify invalid requests are rejected.
 */
static bool thread_test_invalid_arguments(void) {

    if (thread_create(
            "invalid",
            NULL,
            NULL,
            0
        ) != NULL) {

        return false;
    }

    if (thread_create(
            "undersized",
            thread_test_entry,
            NULL,
            THREAD_STACK_MIN_SIZE - 1ULL
        ) != NULL) {

        return false;
    }

    if (thread_create(
            "overflow",
            thread_test_entry,
            NULL,
            SIZE_MAX
        ) != NULL) {

        return false;
    }

    return true;
}


/*
 * Verify thread IDs are unique.
 */
static bool thread_test_unique_ids(void) {

    struct thread *first = thread_create(
        "id-first",
        thread_test_entry,
        NULL,
        0
    );

    if (first == NULL) {
        return false;
    }

    struct thread *second = thread_create(
        "id-second",
        thread_test_entry,
        NULL,
        0
    );

    if (second == NULL) {
        thread_destroy(first);
        return false;
    }

    bool valid =
        first->id != 0 &&
        second->id != 0 &&
        first->id != second->id;

    bool first_destroyed = thread_destroy(first);
    bool second_destroyed = thread_destroy(second);

    return valid && first_destroyed && second_destroyed;
}


/*
 * Verify a thread cannot be destroyed after it leaves
 * the NEW state.
 */
static bool thread_test_destroy_state(void) {

    struct thread *thread = thread_create(
        "destroy-state",
        thread_test_entry,
        NULL,
        0
    );

    if (thread == NULL) {
        return false;
    }

    thread->state = THREAD_STATE_READY;

    bool rejected = !thread_destroy(thread);

    /*
     * Restore NEW so the test can safely release resources.
     */
    thread->state = THREAD_STATE_NEW;

    bool destroyed = thread_destroy(thread);

    return rejected && destroyed;
}

/*
 * Verify real-time thread configuration and validation.
 */
static bool thread_test_realtime_configuration(void) {

    struct thread *thread = thread_create(
        "realtime-test",
        thread_test_entry,
        NULL,
        0
    );

    if (thread == NULL) {
        return false;
    }

    /*
     * Invalid configurations must not partially modify
     * the original fair thread.
     */
    bool invalid_rejected =
        !thread_set_realtime(
            thread,
            THREAD_RT_PRIORITY_LEVELS,
            THREAD_RT_FIFO,
            0
        ) &&
        !thread_set_realtime(
            thread,
            10,
            THREAD_RT_FIFO,
            1
        ) &&
        !thread_set_realtime(
            thread,
            10,
            THREAD_RT_ROUND_ROBIN,
            THREAD_RT_QUANTUM_MIN_NS - 1
        ) &&
        !thread_set_realtime(
            thread,
            10,
            THREAD_RT_ROUND_ROBIN,
            THREAD_RT_QUANTUM_MAX_NS + 1
        ) &&
        !thread_set_realtime(
            thread,
            10,
            (thread_rt_policy_t)99,
            0
        );

    bool unchanged =
        thread->scheduling_class == THREAD_CLASS_FAIR &&
        thread->realtime_priority == 0 &&
        thread->realtime_quantum_ns == 0;

    /*
     * Zero selects the default RR quantum.
     */
    bool rr_configured =
        thread_set_realtime(
            thread,
            31,
            THREAD_RT_ROUND_ROBIN,
            0
        );

    bool rr_valid =
        thread->scheduling_class == THREAD_CLASS_REALTIME &&
        thread->realtime_priority == 31 &&
        thread->realtime_policy == THREAD_RT_ROUND_ROBIN &&
        thread->realtime_quantum_ns ==
            THREAD_RT_DEFAULT_QUANTUM_NS;

    bool destroyed = thread_destroy(thread);

    if (!invalid_rejected ||
        !unchanged ||
        !rr_configured ||
        !rr_valid ||
        !destroyed) {

        return false;
    }

    /*
     * Verify explicit RR quantum configuration.
     */
    thread = thread_create(
        "realtime-rr-explicit",
        thread_test_entry,
        NULL,
        0
    );

    if (thread == NULL) {
        return false;
    }

    bool explicit_rr =
        thread_set_realtime(
            thread,
            12,
            THREAD_RT_ROUND_ROBIN,
            THREAD_RT_QUANTUM_MIN_NS
        ) &&
        thread->realtime_quantum_ns ==
            THREAD_RT_QUANTUM_MIN_NS;

    destroyed = thread_destroy(thread);

    if (!explicit_rr || !destroyed) {
        return false;
    }

    /*
     * FIFO has no quantum.
     */
    thread = thread_create(
        "realtime-fifo",
        thread_test_entry,
        NULL,
        0
    );

    if (thread == NULL) {
        return false;
    }

    bool fifo =
        thread_set_realtime(
            thread,
            20,
            THREAD_RT_FIFO,
            0
        ) &&
        thread->scheduling_class == THREAD_CLASS_REALTIME &&
        thread->realtime_priority == 20 &&
        thread->realtime_policy == THREAD_RT_FIFO &&
        thread->realtime_quantum_ns == 0;

    destroyed = thread_destroy(thread);

    return fifo && destroyed;
}

/*
 * Run all thread foundation tests.
 */
bool thread_run_tests(void) {

    if (!thread_test_creation()) {
        return false;
    }

    if (!thread_test_custom_stack()) {
        return false;
    }

    if (!thread_test_default_name()) {
        return false;
    }

    if (!thread_test_invalid_arguments()) {
        return false;
    }

    if (!thread_test_unique_ids()) {
        return false;
    }

    if (!thread_test_destroy_state()) {
        return false;
    }

    if (!thread_test_realtime_configuration()) {
        return false;
    }

    return true;
}
