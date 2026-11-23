#ifndef HUA_USER_MEMORY_TESTS_H
#define HUA_USER_MEMORY_TESTS_H

#include <stdbool.h>
#include <stdint.h>

#include "mm/vmm.h"

/*
 * Run user-memory validation and copy tests against an
 * existing private process address space.
 *
 * The caller must keep the process and address space alive
 * for the duration of the test.
 */
bool user_memory_run_tests(
    vmm_address_space_t *space,
    uint64_t code_user_address,
    uint64_t code_physical_address,
    uint64_t stack_user_base,
    uint64_t stack_user_top,
    uint64_t stack_physical_address
);

#endif
