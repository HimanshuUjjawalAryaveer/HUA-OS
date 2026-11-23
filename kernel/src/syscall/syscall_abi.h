
#ifndef HUA_SYSCALL_ABI_H
#define HUA_SYSCALL_ABI_H

#include <stdint.h>
#include <stddef.h>

/*
 * HUA System Call ABI.
 *
 * These definitions are shared with future user programs.
 * Assigned syscall numbers must never be reused.
 */

#define HUA_SYSCALL_ABI_VERSION 1U

/*
 * Negative syscall results are encoded in RAX.
 */
#define HUA_SYSCALL_ERROR_OPERATION_FAILED \
    ((uint64_t)(int64_t)-1)

#define HUA_SYSCALL_ERROR_BAD_ADDRESS \
    ((uint64_t)(int64_t)-14)

#define HUA_SYSCALL_ERROR_NOT_IMPLEMENTED \
    ((uint64_t)(int64_t)-38)


/*
 * Stable process-state values exposed through the syscall ABI.
 *
 * Do not rely on a private kernel enum in user programs.
 */
typedef enum hua_process_state {

    HUA_PROCESS_STATE_NEW = 0,

    HUA_PROCESS_STATE_RUNNING = 1,

    HUA_PROCESS_STATE_EXITING = 2,

    HUA_PROCESS_STATE_ZOMBIE = 3

} hua_process_state_t;

_Static_assert(
    HUA_PROCESS_STATE_NEW == 0,
    "HUA process NEW ABI value changed"
);

_Static_assert(
    HUA_PROCESS_STATE_RUNNING == 1,
    "HUA process RUNNING ABI value changed"
);

_Static_assert(
    HUA_PROCESS_STATE_EXITING == 2,
    "HUA process EXITING ABI value changed"
);

_Static_assert(
    HUA_PROCESS_STATE_ZOMBIE == 3,
    "HUA process ZOMBIE ABI value changed"
);

/*
 * Process-information structure returned to Ring 3.
 *
 * Layout:
 *
 *   Offset 0: 64-bit process ID
 *   Offset 8: 32-bit process state
 *   Offset 12: 32-bit attached thread count
 *
 * Total size: 16 bytes.
 */
typedef struct hua_process_info {

    uint64_t pid;

    uint32_t state;

    uint32_t thread_count;

} hua_process_info_t;


_Static_assert(
    sizeof(hua_process_info_t) == 16,
    "HUA process-info ABI structure must be 16 bytes"
);

_Static_assert(
    offsetof(hua_process_info_t, state) == 8,
    "HUA process-info state offset mismatch"
);

_Static_assert(
    offsetof(hua_process_info_t, thread_count) == 12,
    "HUA process-info thread-count offset mismatch"
);


/*
 * x86-64 syscall register convention:
 *
 * RAX = syscall number
 *
 * RDI = argument 1
 * RSI = argument 2
 * RDX = argument 3
 * R10 = argument 4
 * R8  = argument 5
 * R9  = argument 6
 *
 * RAX = return value
 *
 * RCX and R11 are destroyed by SYSCALL.
 */


/*
 * HUA syscall numbers.
 */
typedef enum hua_syscall_number {

    HUA_SYSCALL_GET_ABI_VERSION = 0,

    HUA_SYSCALL_GET_PROCESS_ID = 1,

    HUA_SYSCALL_THREAD_YIELD = 2,

    /*
     * RDI = pointer to hua_process_info_t.
     *
     * Returns:
     *   0  on success
     *  -1  on internal operation failure
     * -14  on invalid user destination
     */
    HUA_SYSCALL_GET_PROCESS_INFO = 3,

    /*
     * Terminate only the calling thread.
     *
     * RDI = signed 64-bit thread exit status.
     *
     * This syscall never returns to user mode.
     */
    HUA_SYSCALL_THREAD_EXIT = 4,

    /*
     * Terminate the calling process.
     *
     * RDI = signed 64-bit process exit status.
     *
     * Initial implementation supports only processes
     * with exactly one attached thread.
     *
     * Returns:
     *  -1 if process termination cannot be initiated.
     *
     * On success, never returns to user mode.
     */
    HUA_SYSCALL_PROCESS_EXIT = 5,

    /*
     * One greater than the highest assigned syscall.
     */
    HUA_SYSCALL_COUNT = 6

} hua_syscall_number_t;

_Static_assert(
    HUA_SYSCALL_GET_ABI_VERSION == 0,
    "GET_ABI_VERSION syscall number changed"
);

_Static_assert(
    HUA_SYSCALL_GET_PROCESS_ID == 1,
    "GET_PROCESS_ID syscall number changed"
);

_Static_assert(
    HUA_SYSCALL_THREAD_YIELD == 2,
    "THREAD_YIELD syscall number changed"
);

_Static_assert(
    HUA_SYSCALL_GET_PROCESS_INFO == 3,
    "GET_PROCESS_INFO syscall number changed"
);

_Static_assert(
    HUA_SYSCALL_THREAD_EXIT == 4,
    "THREAD_EXIT syscall number changed"
);

_Static_assert(
    HUA_SYSCALL_PROCESS_EXIT == 5,
    "PROCESS_EXIT syscall number changed"
);

_Static_assert(
    HUA_SYSCALL_COUNT == 6,
    "HUA syscall count changed"
);

#endif
