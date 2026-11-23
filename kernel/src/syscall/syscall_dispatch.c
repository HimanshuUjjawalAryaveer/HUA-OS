#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/syscall.h"
#include "arch/x86_64/syscall_frame.h"

#include "mm/vmm.h"
#include "mm/user_memory.h"

#include "process/process.h"

#include "scheduler/scheduler.h"
#include "scheduler/thread.h"

#include "syscall/syscall_abi.h"

/*
 * HUA currently uses four-level paging.
 *
 * User addresses must be in the lower canonical half.
 */
#define HUA_USER_ADDRESS_LIMIT 0x0000800000000000ULL

/*
 * RFLAGS bits that must not be restored to user mode
 * through this initial syscall implementation.
 *
 * IF is intentionally allowed.
 */
#define HUA_SYSCALL_FORBIDDEN_USER_RFLAGS ( \
    (1ULL << 8)  |  /* TF   */ \
    (1ULL << 10) |  /* DF   */ \
    (3ULL << 12) |  /* IOPL */ \
    (1ULL << 14) |  /* NT   */ \
    (1ULL << 16) |  /* RF   */ \
    (1ULL << 17) |  /* VM   */ \
    (1ULL << 18) |  /* AC   */ \
    (1ULL << 19) |  /* VIF  */ \
    (1ULL << 20)    /* VIP  */ \
)

/*
 * Invalid return state is fatal for now.
 *
 * HUA does not yet have the complete user-exception
 * termination path needed to safely kill a process here.
 */
static __attribute__((noreturn))
void arch_syscall_fail_stop(void) {

    asm volatile (
        "cli"
        :
        :
        : "memory"
    );

    for (;;) {
        asm volatile (
            "hlt"
            :
            :
            : "memory"
        );
    }
}

/*
 * Validate the user state that SYSRETQ will restore.
 *
 * Important:
 *     SYSRETQ uses RCX as user RIP.
 *     SYSRETQ uses R11 as user RFLAGS.
 *     Our assembly separately restores user RSP.
 */
bool arch_syscall_validate_user_return_state(
    struct thread *thread,
    const struct arch_syscall_frame *frame
) {

    if (thread == NULL ||
        frame == NULL ||
        thread->process == NULL ||
        thread->address_space == NULL ||
        thread->state != THREAD_STATE_RUNNING) {

        return false;
    }

    /*
     * Confirm that the thread's cached address space
     * still matches its owning process.
     */
    vmm_address_space_t *process_space = NULL;

    if (!process_get_address_space(
            thread->process,
            &process_space
        ) ||
        process_space == NULL ||
        process_space != thread->address_space) {

        return false;
    }

    /*
     * Validate the user instruction pointer.
     */
    if (frame->rcx >= HUA_USER_ADDRESS_LIMIT) {
        return false;
    }

    /*
     * Validate the user stack pointer.
     */
    if (frame->user_rsp == 0 ||
        frame->user_rsp >= HUA_USER_ADDRESS_LIMIT) {

        return false;
    }

    /*
     * Bit 1 of RFLAGS must be set.
     *
     * Privileged and unsupported state is rejected.
     * IF remains valid for user mode.
     */
    if ((frame->r11 & 2ULL) == 0 ||
        (frame->r11 &
         HUA_SYSCALL_FORBIDDEN_USER_RFLAGS) != 0) {

        return false;
    }

    /*
     * The return RIP must be:
     *     user accessible
     *     executable
     *     not writable
     */
    vmm_mapping_info_t code_mapping = {0};

    if (vmm_address_space_query(
            thread->address_space,
            frame->rcx,
            &code_mapping
        ) != VMM_STATUS_OK ||
        !code_mapping.user_accessible ||
        !code_mapping.executable ||
        code_mapping.writable) {

        return false;
    }

    /*
     * The byte immediately below the user's RSP must be
     * in a user-writable, non-executable mapping.
     */
    vmm_mapping_info_t stack_mapping = {0};

    if (vmm_address_space_query(
            thread->address_space,
            frame->user_rsp - 1ULL,
            &stack_mapping
        ) != VMM_STATUS_OK ||
        !stack_mapping.user_accessible ||
        !stack_mapping.writable ||
        stack_mapping.executable) {

        return false;
    }

    return true;
}

/*
 * The syscall frame itself must be exactly where the
 * assembly entry routine placed it.
 *
 * This also lets us recover the original entry stack top:
 *
 *     entry_stack_top = frame_address + sizeof(frame)
 *
 * That value must be restored after a syscall that yields.
 */
static uintptr_t arch_syscall_get_frame_stack_top(
    const struct arch_syscall_frame *frame
) {

    if (frame == NULL) {
        return 0;
    }

    uintptr_t configured_top = 0;

    if (!arch_syscall_get_kernel_stack(&configured_top) ||
        configured_top < sizeof(*frame)) {

        return 0;
    }

    uintptr_t expected_frame =
        configured_top - sizeof(*frame);

    if ((uintptr_t)frame != expected_frame) {
        return 0;
    }

    return configured_top;
}

/*
 * Restore the stack configuration belonging to this
 * particular syscall entry.
 *
 * This is essential after thread_yield():
 *
 * The scheduler may have changed both TSS.RSP0 and
 * arch_syscall_kernel_stack_top while another thread ran.
 *
 * The suspended syscall frame is still on its original
 * entry stack, so both values must be restored before SYSRETQ.
 */
static bool arch_syscall_restore_entry_stack(
    const struct arch_syscall_frame *frame
) {

    uintptr_t frame_address =
        (uintptr_t)frame;

    if (frame_address > UINTPTR_MAX - sizeof(*frame)) {
        return false;
    }

    uintptr_t entry_stack_top =
        frame_address + sizeof(*frame);

    /*
     * Restore the hardware privilege-transition stack first.
     */
    if (!gdt_set_kernel_stack(entry_stack_top)) {
        return false;
    }

    /*
     * Keep the software SYSCALL entry stack identical
     * to the currently configured TSS.RSP0.
     */
    if (!arch_syscall_set_kernel_stack(entry_stack_top)) {
        return false;
    }

    uintptr_t observed_tss_stack = 0;
    uintptr_t observed_syscall_stack = 0;

    if (!gdt_get_kernel_stack(&observed_tss_stack) ||
        !arch_syscall_get_kernel_stack(&observed_syscall_stack) ||
        observed_tss_stack != entry_stack_top ||
        observed_syscall_stack != entry_stack_top) {

        return false;
    }

    return true;
}

/*
 * Called directly from syscall_entry.asm.
 *
 * The frame is owned by the calling thread's kernel
 * execution context. It remains valid while the thread
 * is suspended inside thread_yield().
 */
uint64_t arch_syscall_dispatch(
    struct arch_syscall_frame *frame
) {

    if (frame == NULL ||
        !arch_syscall_state_is_initialized()) {

        arch_syscall_fail_stop();
    }

    /*
     * This initial implementation is CPU0-only.
     */
    struct thread *current =
        scheduler_get_current_thread(0);

    if (current == NULL ||
        current->state != THREAD_STATE_RUNNING ||
        current->process == NULL ||
        current->address_space == NULL) {

        arch_syscall_fail_stop();
    }

    /*
     * Validate that the assembly-created frame is on
     * the configured trusted syscall-entry stack.
     */
    if (arch_syscall_get_frame_stack_top(frame) == 0) {
        arch_syscall_fail_stop();
    }

    /*
     * ---------------------------------------------------------
     * THREAD_EXIT: non-returning syscall
     * ---------------------------------------------------------
     *
     * A terminating thread does not need a valid user return
     * state because it must never return to user mode.
     *
     * The trusted syscall frame and the current process-owned
     * thread have already been validated above.
     *
     * Do not:
     * - Validate a user return state.
     * - Restore the original syscall-entry stack.
     * - Return a result to syscall_entry.asm.
     * - Execute SYSRET.
     *
     * thread_exit() switches permanently to another thread.
     */
    if (frame->rax == HUA_SYSCALL_THREAD_EXIT) {

        thread_exit((int64_t)frame->rdi);

        /*
         * thread_exit() is declared _Noreturn.
         * This additionally documents the control-flow invariant.
         */
        __builtin_unreachable();
    }

    /*
     * Returning syscalls must have a valid user return state.
     */
    if (!arch_syscall_validate_user_return_state(current, frame)) {
        arch_syscall_fail_stop();
    }

    uint64_t result =
        HUA_SYSCALL_ERROR_NOT_IMPLEMENTED;

    switch (frame->rax) {

        /*
         * Return the HUA syscall ABI version.
         */
        case HUA_SYSCALL_GET_ABI_VERSION:

            result = HUA_SYSCALL_ABI_VERSION;

            break;

        /*
         * Return the PID belonging to the calling thread's
         * owning process.
         *
         * The caller does not supply a PID.
         */
        case HUA_SYSCALL_GET_PROCESS_ID: {

            process_id_t pid = 0;

            if (!process_get_info(
                    current->process,
                    &pid,
                    NULL,
                    NULL,
                    NULL,
                    0
                ) ||
                pid == 0) {

                result =
                    HUA_SYSCALL_ERROR_OPERATION_FAILED;

            } else {

                result = (uint64_t)pid;
            }

            break;
        }

        /*
         * Retrieve information about the calling process.
         *
         * RDI contains an untrusted user virtual address.
         * Never dereference it directly.
         */
        case HUA_SYSCALL_GET_PROCESS_INFO: {

            process_id_t pid = 0;

            process_state_t state = PROCESS_STATE_NEW;

            uint32_t thread_count = 0;

            /*
             * Obtain one consistent process-information
             * snapshot under the process manager's lock.
             */
            if (!process_get_info(
                    current->process,
                    &pid,
                    &state,
                    &thread_count,
                    NULL,
                    0
                ) ||
                pid == 0) {

                result =
                    HUA_SYSCALL_ERROR_OPERATION_FAILED;

                break;
            }

            /*
             * Translate private kernel states into stable
             * public syscall ABI values.
             *
             * Never expose the private enum by blindly
             * casting it into the public ABI.
             */
            uint32_t abi_state = 0;

            bool state_valid = true;

            switch (state) {

                case PROCESS_STATE_NEW:

                    abi_state = HUA_PROCESS_STATE_NEW;

                    break;

                case PROCESS_STATE_RUNNING:

                    abi_state = HUA_PROCESS_STATE_RUNNING;

                    break;

                case PROCESS_STATE_EXITING:

                    abi_state = HUA_PROCESS_STATE_EXITING;

                    break;

                case PROCESS_STATE_ZOMBIE:

                    abi_state = HUA_PROCESS_STATE_ZOMBIE;

                    break;

                default:

                    state_valid = false;

                    break;
            }

            if (!state_valid) {

                result =
                    HUA_SYSCALL_ERROR_OPERATION_FAILED;

                break;
            }

            /*
             * Construct the ABI object entirely in
             * kernel-owned memory.
             */
            hua_process_info_t info = {

                .pid = (uint64_t)pid,

                .state = abi_state,

                .thread_count = thread_count

            };

            /*
             * Copy the complete structure into the
             * calling process's address space.
             */
            user_memory_status_t copy_status =
                copy_to_user(
                    current->address_space,
                    frame->rdi,
                    &info,
                    sizeof(info)
                );

            /*
             * Distinguish invalid user memory from an
             * internal kernel/user-memory subsystem failure.
             *
             * A bad user destination returns -14.
             * An internal operation failure returns -1.
             */
            switch (copy_status) {

                case USER_MEMORY_STATUS_OK:

                    result = 0;

                    break;

                case USER_MEMORY_STATUS_RANGE_OVERFLOW:

                case USER_MEMORY_STATUS_NOT_USER_RANGE:

                case USER_MEMORY_STATUS_NOT_MAPPED:

                case USER_MEMORY_STATUS_NOT_USER_ACCESSIBLE:

                case USER_MEMORY_STATUS_NOT_WRITABLE:

                    result =
                        HUA_SYSCALL_ERROR_BAD_ADDRESS;

                    break;

                default:

                    result =
                        HUA_SYSCALL_ERROR_OPERATION_FAILED;

                    break;
            }

            break;
        }

        /*
         * Voluntarily yield the current thread.
         *
         * thread_yield() returns to this dispatcher only
         * after this thread is scheduled again.
         */
        case HUA_SYSCALL_THREAD_YIELD:

            if (thread_yield()) {
                result = 0;
            } else {
                result =
                    HUA_SYSCALL_ERROR_OPERATION_FAILED;
            }

            /*
             * This syscall's C context must resume on
             * the same thread that entered it.
             */
            if (scheduler_get_current_thread(0) != current ||
                current->state != THREAD_STATE_RUNNING) {

                arch_syscall_fail_stop();
            }

            break;

        /*
         * Terminate the calling process.
         *
         * The process manager authorizes termination only
         * when this is the process's sole attached thread.
         */
        case HUA_SYSCALL_PROCESS_EXIT:

            if (!process_begin_exit_from_thread(
                    current->process,
                    current
                )) {

                /*
                 * Rejection leaves the process and thread alive.
                 */
                result =
                    HUA_SYSCALL_ERROR_OPERATION_FAILED;

                break;
            }

            /*
             * Successful authorization changes the process
             * to EXITING. The final thread is then terminated
             * and reclaimed by the deferred reaper.
             */
            thread_exit((int64_t)frame->rdi);

            __builtin_unreachable();

        default:

            /*
             * Unknown syscall numbers return -38.
             */
            result =
                HUA_SYSCALL_ERROR_NOT_IMPLEMENTED;

            break;
    }

    /*
     * A yielding syscall may have switched to other threads.
     *
     * Restore the original syscall stack before returning
     * to the assembly entry routine.
     */
    if (!arch_syscall_restore_entry_stack(frame)) {
        arch_syscall_fail_stop();
    }

    return result;
}
