#include <stdbool.h>
#include <stdint.h>

#include "arch/x86_64/msr.h"
#include "tests/msr_tests.h"


/*
 * Verify that the common MSR access interface works.
 *
 * HUA is already executing in x86-64 long mode.
 * Consequently IA32_EFER.LMA must be set.
 */
bool msr_run_tests(void) {

    uint64_t efer =
        arch_msr_read(ARCH_MSR_IA32_EFER);

    /*
     * Long Mode Active must be set.
     */
    if (!(efer & ARCH_EFER_LMA)) {
        return false;
    }

    /*
     * Long Mode Enable must also remain set.
     */
    if (!(efer & ARCH_EFER_LME)) {
        return false;
    }

    return true;
}
