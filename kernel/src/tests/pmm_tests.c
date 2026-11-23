#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "mm/pmm.h"
#include "drivers/console/console.h"

#include "pmm_tests.h"

#define PMM_STRESS_COUNT 32

static void pmm_test_failure(const char *message)
{
    console_write("[ FAILED ] ");
    console_write(message);
    console_write("\n");
}

static void pmm_test_success(const char *message)
{
    console_write("[ OK ] ");
    console_write(message);
    console_write("\n");
}

static void pmm_release_frames(uint64_t *frames, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++) {
        if (frames[i] != 0) {
            pmm_free_frame(frames[i]);
        }
    }
}

static bool pmm_test_free_failure_countdown(void)
{
    /*
     * Start with both failure-injection modes disabled.
     */
    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    uint64_t free_before = pmm_get_free_frames();

    uint64_t first_frame = pmm_alloc_frame();
    uint64_t second_frame = pmm_alloc_frame();

    bool first_owned = first_frame != 0;
    bool second_owned = second_frame != 0;

    bool passed = false;

    if (!first_owned || !second_owned) {
        goto cleanup;
    }

    /*
     * Allow exactly one successful free, then fail the next.
     */
    if (!pmm_test_fail_free_after(1)) {
        goto cleanup;
    }

    /*
     * The first release must succeed and consume the countdown.
     */
    if (!pmm_free_frame(first_frame)) {
        goto cleanup;
    }

    first_owned = false;

    if (pmm_frame_to_virt(first_frame) != NULL) {
        goto cleanup;
    }

    /*
     * The second release must fail exactly once.
     * Its physical frame must remain allocated.
     */
    if (pmm_free_frame(second_frame)) {
        second_owned = false;
        goto cleanup;
    }

    if (pmm_frame_to_virt(second_frame) == NULL) {
        goto cleanup;
    }

    /*
     * The failure is one-shot. The retry must succeed without
     * explicitly clearing the countdown hook.
     */
    if (!pmm_free_frame(second_frame)) {
        goto cleanup;
    }

    second_owned = false;

    if (pmm_frame_to_virt(second_frame) != NULL) {
        goto cleanup;
    }

    passed = true;

cleanup:

    /*
     * Never leave test injection active, even on an early exit.
     */
    pmm_test_clear_free_frame_failure();
    pmm_test_clear_free_after_failure();

    /*
     * Release anything the test still owns.
     */
    if (first_owned && pmm_free_frame(first_frame)) {
        first_owned = false;
    }

    if (second_owned && pmm_free_frame(second_frame)) {
        second_owned = false;
    }

    /*
     * Verify both cleanup and PMM accounting.
     */
    return passed &&
        !first_owned &&
        !second_owned &&
        pmm_get_free_frames() == free_before;
}

bool pmm_run_tests(void)
{
    console_write("\n===== PMM SELF-TESTS =====\n");

    /*
     * Test 1:
     * Physical frame allocation and freeing.
     */

    uint64_t frames_before = pmm_get_free_frames();

    uint64_t frame_a = pmm_alloc_frame();
    uint64_t frame_b = pmm_alloc_frame();

    if (frame_a == 0 || frame_b == 0) {
        pmm_test_failure("Frame allocation test");

        if (frame_a != 0) {
            pmm_free_frame(frame_a);
        }

        if (frame_b != 0) {
            pmm_free_frame(frame_b);
        }

        return false;
    }

    /*
     * Test 2:
     * Verify that allocated frames are zero-initialized.
     */

    void *zero_test_virtual = pmm_frame_to_virt(frame_a);

    if (zero_test_virtual == NULL) {
        pmm_test_failure("Zero-initialization address test");

        pmm_free_frame(frame_a);
        pmm_free_frame(frame_b);

        return false;
    }

    volatile uint64_t *zero_test_words =
        (volatile uint64_t *)zero_test_virtual;

    for (uint64_t i = 0;
         i < PMM_PAGE_SIZE / sizeof(uint64_t);
         i++) {

        if (zero_test_words[i] != 0) {
            pmm_test_failure("Frame zero-initialization test");

            pmm_free_frame(frame_a);
            pmm_free_frame(frame_b);

            return false;
        }
    }

    pmm_test_success("Physical frame zero-initialization");

    /*
     * Test 3:
     * Verify physical frame read/write access through HHDM.
     */

    void *frame_a_virtual = pmm_frame_to_virt(frame_a);

    if (frame_a_virtual == NULL) {
        pmm_test_failure("Frame virtual address conversion");

        pmm_free_frame(frame_a);
        pmm_free_frame(frame_b);

        return false;
    }

    volatile uint64_t *frame_words =
        (volatile uint64_t *)frame_a_virtual;

    uint64_t test_pattern_a = 0x1122334455667788ULL;
    uint64_t test_pattern_b = 0xAABBCCDDEEFF0011ULL;

    frame_words[0] = test_pattern_a;

    frame_words[(PMM_PAGE_SIZE / sizeof(uint64_t)) - 1] =
        test_pattern_b;

    if (frame_words[0] != test_pattern_a ||
        frame_words[(PMM_PAGE_SIZE / sizeof(uint64_t)) - 1]
            != test_pattern_b) {

        pmm_test_failure("Physical frame read/write test");

        frame_words[0] = 0;

        frame_words[(PMM_PAGE_SIZE / sizeof(uint64_t)) - 1] = 0;

        pmm_free_frame(frame_a);
        pmm_free_frame(frame_b);

        return false;
    }

    frame_words[0] = 0;

    frame_words[(PMM_PAGE_SIZE / sizeof(uint64_t)) - 1] = 0;

    pmm_test_success("Physical frame read/write");

    /*
     * Test 4:
     * Verify frame freeing and double-free protection.
     */

    uint64_t frames_after_alloc = pmm_get_free_frames();

    bool freed_a = pmm_free_frame(frame_a);
    bool freed_b = pmm_free_frame(frame_b);

    bool double_free_rejected = !pmm_free_frame(frame_a);

    uint64_t frames_after_free = pmm_get_free_frames();

    if (!freed_a ||
        !freed_b ||
        !double_free_rejected ||
        frames_after_alloc != frames_before - 2 ||
        frames_after_free != frames_before) {

        pmm_test_failure("PMM allocation/free accounting");

        return false;
    }

    pmm_test_success("Both frames freed successfully");
    pmm_test_success("Double-free protection");
    pmm_test_success("Free-frame count restored");

    /*
     * Test 5:
     * Reject invalid physical frame addresses.
     */

    bool zero_address_rejected =
        !pmm_free_frame(0);

    bool unaligned_address_rejected =
        !pmm_free_frame(0x1001);

    bool reserved_frame_rejected =
        !pmm_free_frame(0x1000);

    if (!zero_address_rejected ||
        !unaligned_address_rejected ||
        !reserved_frame_rejected ||
        pmm_get_free_frames() != frames_before) {

        pmm_test_failure("Invalid frame rejection");

        return false;
    }

    pmm_test_success("Zero-address rejection");
    pmm_test_success("Unaligned-address rejection");
    pmm_test_success("Reserved-frame rejection");

    /*
     * Test 6:
     * Stress-test multiple physical frame allocations.
     */

    uint64_t stress_frames[PMM_STRESS_COUNT] = {0};

    uint64_t stress_free_before = pmm_get_free_frames();

    uint64_t stress_allocated = 0;

    /*
     * Allocate 32 physical frames.
     */

    for (uint64_t i = 0; i < PMM_STRESS_COUNT; i++) {

        stress_frames[i] = pmm_alloc_frame();

        if (stress_frames[i] == 0) {
            pmm_test_failure("PMM stress allocation");

            pmm_release_frames(stress_frames, stress_allocated);

            return false;
        }

        /*
         * Every frame must be 4 KiB aligned.
         */

        if ((stress_frames[i] & (PMM_PAGE_SIZE - 1)) != 0) {
            pmm_test_failure("Frame alignment");

            pmm_release_frames(stress_frames, stress_allocated + 1);

            return false;
        }

        /*
         * No two allocations may return the same frame.
         */

        for (uint64_t j = 0; j < i; j++) {

            if (stress_frames[i] == stress_frames[j]) {
                pmm_test_failure("Duplicate frame allocation");

                pmm_release_frames(stress_frames, stress_allocated + 1);

                return false;
            }
        }

        stress_allocated++;
    }

    pmm_test_success("32-frame allocation");

    pmm_test_success("No duplicate physical frames");

    /*
     * Release all allocated frames.
     */

    bool all_frames_freed = true;

    for (uint64_t i = 0; i < PMM_STRESS_COUNT; i++) {

        if (!pmm_free_frame(stress_frames[i])) {
            all_frames_freed = false;
        }
    }

    if (!all_frames_freed) {
        pmm_test_failure("PMM stress frame freeing");

        return false;
    }

    pmm_test_success("All stress-test frames freed");

    /*
     * Verify free-frame accounting.
     */

    if (pmm_get_free_frames() != stress_free_before) {
        pmm_test_failure("PMM stress accounting");

        return false;
    }

    pmm_test_success("PMM stress accounting restored");

    bool countdown_failure_test =
        pmm_test_free_failure_countdown();

    if (!countdown_failure_test) {
        pmm_test_failure(
            "Countdown-based frame-free failure and retry"
        );

        return false;
    }

    pmm_test_success(
        "Countdown-based frame-free failure and retry"
    );

    console_write("===== PMM SELF-TESTS PASSED =====\n");

    return true;
}
