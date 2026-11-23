
#include "scheduler/fair.h"

/*
 * Check whether a fair scheduling weight is supported.
 */
static bool scheduler_fair_weight_valid(uint64_t weight) {

    return weight >= THREAD_FAIR_WEIGHT_MIN &&
           weight <= THREAD_FAIR_WEIGHT_MAX;
}


/*
 * Checked unsigned addition.
 */
static bool scheduler_fair_add_u64(
    uint64_t a,
    uint64_t b,
    uint64_t *result
) {

    if (result == NULL || a > UINT64_MAX - b) {
        return false;
    }

    *result = a + b;

    return true;
}


/*
 * Set the weight of a newly created fair thread.
 */
bool scheduler_fair_set_weight(
    struct thread *thread,
    uint64_t weight
) {

    if (thread == NULL ||
        thread->scheduling_class != THREAD_CLASS_FAIR ||
        thread->state != THREAD_STATE_NEW ||
        thread->run_queue_owner != NULL ||
        !scheduler_fair_weight_valid(weight)) {

        return false;
    }

    thread->fair_weight = weight;

    return true;
}


/*
 * Account elapsed execution time.
 *
 * Formula:
 *
 * virtual_delta =
 *     elapsed_ns * THREAD_FAIR_WEIGHT_SCALE / fair_weight
 *
 * The calculation is decomposed into quotient and
 * remainder so that elapsed_ns * 1024 cannot overflow.
 */
bool scheduler_fair_account_runtime(
    struct thread *thread,
    uint64_t elapsed_ns
) {

    if (thread == NULL ||
        thread->scheduling_class != THREAD_CLASS_FAIR ||
        !scheduler_fair_weight_valid(thread->fair_weight)) {

        return false;
    }

    if (elapsed_ns == 0) {
        return true;
    }

    uint64_t new_runtime;

    if (!scheduler_fair_add_u64(
            thread->runtime_ns,
            elapsed_ns,
            &new_runtime
        )) {

        return false;
    }

    uint64_t quotient =
        elapsed_ns / thread->fair_weight;

    uint64_t remainder =
        elapsed_ns % thread->fair_weight;

    if (quotient >
        UINT64_MAX / THREAD_FAIR_WEIGHT_SCALE) {

        return false;
    }

    uint64_t virtual_delta =
        quotient * THREAD_FAIR_WEIGHT_SCALE;

    /*
     * remainder is less than the maximum weight,
     * so this multiplication is safely bounded.
     */
    uint64_t fractional_delta =
        (remainder * THREAD_FAIR_WEIGHT_SCALE) /
        thread->fair_weight;

    if (!scheduler_fair_add_u64(
            virtual_delta,
            fractional_delta,
            &virtual_delta
        )) {

        return false;
    }

    /*
     * Preserve forward progress for very small execution
     * intervals with a high scheduling weight.
     */
    if (virtual_delta == 0) {
        virtual_delta = 1;
    }

    uint64_t new_virtual_runtime;

    if (!scheduler_fair_add_u64(
            thread->virtual_runtime,
            virtual_delta,
            &new_virtual_runtime
        )) {

        return false;
    }

    /*
     * Commit only after all overflow checks succeed.
     */
    thread->runtime_ns = new_runtime;
    thread->virtual_runtime = new_virtual_runtime;

    return true;
}


/*
 * Calculate the virtual deadline for a scheduling request.
 *
 * Formula:
 *
 * virtual_request =
 *     fair_slice_ns * 1024 / fair_weight
 *
 * virtual_deadline =
 *     virtual_runtime + virtual_request
 */
bool scheduler_fair_update_deadline(
    struct thread *thread
) {

    if (thread == NULL ||
        thread->scheduling_class != THREAD_CLASS_FAIR ||
        !scheduler_fair_weight_valid(thread->fair_weight) ||
        thread->fair_slice_ns == 0) {

        return false;
    }

    if (thread->fair_slice_ns >
        UINT64_MAX / THREAD_FAIR_WEIGHT_SCALE) {

        return false;
    }

    uint64_t virtual_request =
        (thread->fair_slice_ns *
         THREAD_FAIR_WEIGHT_SCALE) /
        thread->fair_weight;

    if (virtual_request == 0) {
        virtual_request = 1;
    }

    uint64_t deadline;

    if (!scheduler_fair_add_u64(
            thread->virtual_runtime,
            virtual_request,
            &deadline
        )) {

        return false;
    }

    thread->virtual_deadline = deadline;

    return true;
}


/*
 * Recalculate the weighted average virtual runtime.
 *
 * We first find:
 *
 *     min_vruntime = minimum runnable virtual runtime
 *
 * Then calculate:
 *
 *     avg_vruntime =
 *         min_vruntime +
 *         sum((vruntime - min_vruntime) * weight)
 *         / sum(weight)
 *
 * The offset calculation is split into quotient and
 * remainder to avoid overflowing uint64_t.
 */
void scheduler_fair_recalculate(
    struct scheduler_run_queue *queue
) {

    if (queue == NULL) {
        return;
    }

    struct scheduler_thread_queue *fair = &queue->fair;

    if (fair->head == NULL) {

        queue->fair_min_vruntime = 0;
        queue->fair_avg_vruntime = 0;
        queue->fair_total_weight = 0;

        return;
    }

    uint64_t minimum = UINT64_MAX;
    uint64_t total_weight = 0;

    /*
     * First pass: find minimum virtual runtime and
     * validate the total scheduling weight.
     */
    for (struct thread *thread = fair->head;
         thread != NULL;
         thread = thread->run_next) {

        if (!scheduler_fair_weight_valid(thread->fair_weight) ||
            total_weight >
                SCHEDULER_FAIR_MAX_RUNQUEUE_WEIGHT -
                thread->fair_weight) {

            /*
             * An invalid queue should never be produced
             * by the public enqueue API.
             *
             * Reset aggregates rather than publishing
             * a partially calculated average.
             */
            queue->fair_min_vruntime = 0;
            queue->fair_avg_vruntime = 0;
            queue->fair_total_weight = 0;

            return;
        }

        total_weight += thread->fair_weight;

        if (thread->virtual_runtime < minimum) {
            minimum = thread->virtual_runtime;
        }
    }

    if (total_weight == 0 || minimum == UINT64_MAX) {

        queue->fair_min_vruntime = 0;
        queue->fair_avg_vruntime = 0;
        queue->fair_total_weight = 0;

        return;
    }

    /*
    * Second pass: calculate the exact weighted average
    * using quotient/remainder accumulation.
    *
    * We preserve fractional remainders between threads
    * instead of truncating each thread independently.
    */
    uint64_t weighted_offset = 0;
    uint64_t fractional_remainder = 0;

    for (struct thread *thread = fair->head;
        thread != NULL;
        thread = thread->run_next) {

        uint64_t delta =
            thread->virtual_runtime - minimum;

        uint64_t quotient = delta / total_weight;
        uint64_t remainder = delta % total_weight;

        /*
        * quotient * weight cannot exceed delta.
        */
        uint64_t contribution =
            quotient * thread->fair_weight;

        /*
        * remainder * weight is bounded by 2^60.
        */
        uint64_t fractional_product =
            remainder * thread->fair_weight;

        contribution +=
            fractional_product / total_weight;

        uint64_t new_remainder =
            fractional_product % total_weight;

        /*
        * Carry fractional contributions forward.
        * Both remainders are smaller than total_weight.
        */
        fractional_remainder += new_remainder;

        if (fractional_remainder >= total_weight) {

            fractional_remainder -= total_weight;

            contribution++;
        }

        if (weighted_offset > UINT64_MAX - contribution) {

            queue->fair_min_vruntime = minimum;
            queue->fair_avg_vruntime = minimum;
            queue->fair_total_weight = total_weight;

            return;
        }

        weighted_offset += contribution;
    }

    queue->fair_min_vruntime = minimum;
    queue->fair_avg_vruntime = minimum + weighted_offset;
    queue->fair_total_weight = total_weight;
}


/*
 * Select an eligible fair thread using EEVDF.
 *
 * Eligibility:
 *
 *     thread.virtual_runtime <= queue.fair_avg_vruntime
 *
 * Among eligible threads, select the earliest virtual
 * deadline.
 *
 * Tie-breaking:
 *     1. Lower virtual runtime.
 *     2. Lower thread ID.
 */
struct thread *scheduler_fair_pick(
    struct scheduler_run_queue *queue
) {

    if (queue == NULL ||
        queue->fair.head == NULL ||
        queue->fair.count == 0 ||
        queue->fair_total_weight == 0) {

        return NULL;
    }

    struct thread *best = NULL;

    for (struct thread *candidate = queue->fair.head;
         candidate != NULL;
         candidate = candidate->run_next) {

        /*
         * A thread ahead of the weighted average has
         * already received more than its fair share.
         */
        if (candidate->virtual_runtime >
            queue->fair_avg_vruntime) {

            continue;
        }

        if (best == NULL) {
            best = candidate;
            continue;
        }

        bool earlier_deadline =
            candidate->virtual_deadline <
            best->virtual_deadline;

        bool equal_deadline =
            candidate->virtual_deadline ==
            best->virtual_deadline;

        bool lower_runtime =
            candidate->virtual_runtime <
            best->virtual_runtime;

        bool equal_runtime =
            candidate->virtual_runtime ==
            best->virtual_runtime;

        bool lower_id =
            candidate->id < best->id;

        if (earlier_deadline ||
            (equal_deadline && lower_runtime) ||
            (equal_deadline && equal_runtime && lower_id)) {

            best = candidate;
        }
    }

    /*
     * At least one thread should be eligible because
     * the weighted average lies within the range of
     * the runnable virtual runtimes.
     */
    return best;
}

/*
 * Check whether a fair thread has consumed its
 * scheduling request.
 */
bool scheduler_fair_slice_expired(
    const struct thread *thread
) {

    if (thread == NULL ||
        thread->scheduling_class != THREAD_CLASS_FAIR ||
        thread->fair_slice_ns == 0) {

        return false;
    }

    return thread->slice_runtime_ns >=
           thread->fair_slice_ns;
}


/*
 * Determine whether an expired fair thread should
 * give the CPU to another fair thread.
 */
bool scheduler_fair_should_preempt(
    const struct thread *thread,
    const struct scheduler_run_queue *queue
) {

    if (queue == NULL ||
        !scheduler_fair_slice_expired(thread)) {

        return false;
    }

    return queue->fair.head != NULL;
}
