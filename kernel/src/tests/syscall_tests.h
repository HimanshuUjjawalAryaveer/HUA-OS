#ifndef HUA_SYSCALL_TESTS_H
#define HUA_SYSCALL_TESTS_H

#include <stdbool.h>
#include <stdint.h>

struct thread;

bool syscall_user_return_validation_run_tests(
    struct thread *thread,
    uintptr_t user_code,
    uintptr_t user_stack_top
);

bool syscall_run_tests(void);

bool syscall_state_run_tests(void);

#endif
