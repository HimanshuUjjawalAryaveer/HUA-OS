#include "arch/x86_64/ring3.h"
#include "arch/x86_64/interrupt_frame.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "arch/x86_64/gdt.h"

#include "process/process.h"
#include "scheduler/scheduler.h"

#include "mm/vmm.h"


/*
 * This variable is accessed by ring3.asm.
 *
 * It contains the saved kernel RSP after the assembly
 * entry function has saved the callee-saved registers.
 *
 * Only CPU0 can use this transition mechanism.
 */
uintptr_t ring3_saved_kernel_rsp = 0;


/*
 * RFLAGS bits.
 */
#define RING3_RFLAGS_TF       (1ULL << 8)
#define RING3_RFLAGS_IF       (1ULL << 9)
#define RING3_RFLAGS_DF       (1ULL << 10)
#define RING3_RFLAGS_IOPL     (3ULL << 12)
#define RING3_RFLAGS_NT       (1ULL << 14)
#define RING3_RFLAGS_VM       (1ULL << 17)


/*
 * CR3 physical-address mask.
 *
 * The low 12 bits contain control information or PCID.
 * The page-table root address is page-aligned.
 */
#define RING3_CR3_ADDRESS_MASK 0x000FFFFFFFFFF000ULL


/*
 * Number of bytes pushed by arch_ring3_enter_user()
 * before recording its saved kernel RSP.
 *
 * RBX, RBP, R12, R13, R14, R15.
 */
#define RING3_SAVED_REGISTER_BYTES (6ULL * sizeof(uint64_t))

_Static_assert(
    sizeof(struct interrupt_frame) == 22ULL * sizeof(uint64_t),
    "Ring 3 interrupt-frame layout does not match assembly"
);

/*
 * Expected state for one controlled Ring 3 transition.
 *
 * The current design is CPU0-only and supports one active
 * test transition at a time.
 */
struct ring3_test_context {

    struct thread *thread;

    struct process *process;

    vmm_address_space_t *address_space;

    uintptr_t transition_stack_top;

    uintptr_t user_rip;

    uintptr_t user_rsp;

};


/*
 * Active test context.
 *
 * The active flag is published last using release
 * ordering. The interrupt handler acquires it before
 * reading the remaining fields.
 */
static struct ring3_test_context ring3_context;

static uint32_t ring3_test_active = 0;

static uint32_t ring3_test_hit = 0;


/*
 * Read the currently active CR3 physical root.
 */
static uint64_t ring3_read_cr3_root(void) {

    uint64_t cr3 = 0;

    asm volatile (
        "mov %%cr3, %0"
        : "=r"(cr3)
        :
        : "memory"
    );

    return cr3 & RING3_CR3_ADDRESS_MASK;
}


/*
 * Verify that interrupts are disabled.
 */
static bool ring3_interrupts_are_disabled(void) {

    uint64_t rflags = 0;

    asm volatile (
        "pushfq\n"
        "popq %0"
        : "=r"(rflags)
        :
        : "memory"
    );

    return (rflags & RING3_RFLAGS_IF) == 0;
}


/*
 * Arm one controlled Ring 3 transition.
 */
bool ring3_test_arm(
    struct thread *thread,
    uintptr_t transition_stack_top,
    uintptr_t user_rip,
    uintptr_t user_rsp
) {

    if (thread == NULL ||
        transition_stack_top == 0 ||
        user_rip == 0 ||
        user_rsp == 0) {

        return false;
    }

    /*
     * This implementation is intentionally CPU0-only.
     */
    if (!scheduler_is_initialized() ||
        scheduler_get_current_thread(0) != thread) {

        return false;
    }

    /*
     * Interrupts must be disabled throughout the transition.
     */
    if (!ring3_interrupts_are_disabled()) {
        return false;
    }

    /*
     * The thread must own a valid kernel stack.
     */
    if (!thread->stack_owned ||
        thread->stack_base == NULL ||
        thread->stack_size == 0) {

        return false;
    }

    uintptr_t stack_base =
        (uintptr_t)thread->stack_base;

    if (stack_base > UINTPTR_MAX - thread->stack_size) {
        return false;
    }

    if (thread->stack_top !=
            stack_base + thread->stack_size ||
        (thread->stack_top & 15ULL) != 0) {

        return false;
    }

    /*
     * Verify the expected TSS.RSP0 value.
     */
    uintptr_t current_rsp0 = 0;

    if (!gdt_get_kernel_stack(&current_rsp0) ||
        current_rsp0 != transition_stack_top) {

        return false;
    }

    /*
     * Verify the thread belongs to a process and has
     * a valid borrowed address-space pointer.
     */
    if (thread->process == NULL ||
        thread->address_space == NULL) {

        return false;
    }

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
     * The worker must actually be running in its
     * process address space.
     */
    uint64_t expected_root = 0;

    if (!vmm_address_space_get_root_physical(
            process_space,
            &expected_root
        )) {

        return false;
    }

    if (ring3_read_cr3_root() != expected_root) {
        return false;
    }

    /*
    * Verify that the authorized user instruction is mapped:
    *
    *     User accessible
    *     Read-only
    *     Executable
    */
    vmm_mapping_info_t code_mapping = {0};

    if (vmm_address_space_query(
            process_space,
            user_rip,
            &code_mapping
        ) != VMM_STATUS_OK ||
        !code_mapping.user_accessible ||
        code_mapping.writable ||
        !code_mapping.executable) {

        return false;
    }

    /*
    * Verify that the byte immediately below the initial
    * user RSP belongs to a user-writable, NX stack mapping.
    */
    vmm_mapping_info_t stack_mapping = {0};

    if (vmm_address_space_query(
            process_space,
            user_rsp - 1ULL,
            &stack_mapping
        ) != VMM_STATUS_OK ||
        !stack_mapping.user_accessible ||
        !stack_mapping.writable ||
        stack_mapping.executable) {

        return false;
    }

    /*
     * Verify user stack alignment.
     *
     * The initial user program does not call another
     * function. Nevertheless, maintain normal x86-64
     * stack alignment.
     */
    if ((user_rsp & 15ULL) != 0) {
        return false;
    }

    /*
     * Only one controlled transition can be active.
     */
    if (__atomic_load_n(
            &ring3_test_active,
            __ATOMIC_ACQUIRE
        ) != 0) {

        return false;
    }

    if (__atomic_load_n(
            &ring3_saved_kernel_rsp,
            __ATOMIC_ACQUIRE
        ) != 0) {

        return false;
    }

    /*
     * Publish expected state.
     */
    ring3_context.thread = thread;
    ring3_context.process = thread->process;
    ring3_context.address_space = process_space;

    ring3_context.transition_stack_top =
        transition_stack_top;

    ring3_context.user_rip = user_rip;
    ring3_context.user_rsp = user_rsp;

    __atomic_store_n(
        &ring3_test_hit,
        0,
        __ATOMIC_RELAXED
    );

    /*
     * Publish the context only after every field is ready.
     */
    __atomic_store_n(
        &ring3_test_active,
        1,
        __ATOMIC_RELEASE
    );

    return true;
}


/*
 * Handle a user-originated breakpoint.
 *
 * This is deliberately strict.
 *
 * An arbitrary user process must not be able to redirect
 * its interrupt frame to a privileged kernel trampoline.
 */
bool ring3_test_handle_breakpoint(
    struct interrupt_frame *frame
) {

    if (frame == NULL || frame->vector != 3) {
        return false;
    }

    /*
     * Only a genuine CPL3-originated interrupt is accepted.
     */
    if ((frame->cs & 3ULL) != 3ULL) {
        return false;
    }

    /*
     * The controlled test must be active.
     */
    if (__atomic_load_n(
            &ring3_test_active,
            __ATOMIC_ACQUIRE
        ) != 1) {

        return false;
    }

    struct thread *thread =
        scheduler_get_current_thread(0);

    if (thread == NULL ||
        thread != ring3_context.thread ||
        thread->process != ring3_context.process ||
        thread->state != THREAD_STATE_RUNNING ||
        thread->address_space != ring3_context.address_space) {

        return false;
    }

    /*
     * Verify that the interrupted context used the expected
     * user code selector.
     */
    if (frame->cs != GDT_USER_CODE_SELECTOR) {
        return false;
    }

    /*
     * int3 is exactly one byte.
     *
     * The CPU saves RIP pointing to the instruction
     * immediately after int3.
     */
    if (ring3_context.user_rip == UINTPTR_MAX ||
        frame->rip != ring3_context.user_rip + 1ULL) {

        return false;
    }

    /*
     * Verify the user stack information pushed by the CPU.
     *
     * These optional fields are valid because CS.RPL == 3.
     */
    if (frame->old_rsp != ring3_context.user_rsp ||
        frame->old_ss != GDT_USER_DATA_SELECTOR) {

        return false;
    }

    /*
     * This controlled breakpoint should not have an
     * exception error code.
     */
    if (frame->error_code != 0) {
        return false;
    }

    /*
     * Verify that user mode did not enter with privileged
     * or unexpected execution flags.
     */
    const uint64_t forbidden_rflags =
        RING3_RFLAGS_TF |
        RING3_RFLAGS_IF |
        RING3_RFLAGS_DF |
        RING3_RFLAGS_IOPL |
        RING3_RFLAGS_NT |
        RING3_RFLAGS_VM;

    if ((frame->rflags & forbidden_rflags) != 0 ||
        (frame->rflags & 2ULL) == 0) {

        return false;
    }

    /*
     * Verify that the interrupt arrived on the dedicated
     * transition stack, not the worker's suspended stack.
     */
    uintptr_t current_rsp0 = 0;

    if (!gdt_get_kernel_stack(&current_rsp0) ||
        current_rsp0 !=
            ring3_context.transition_stack_top) {

        return false;
    }

    /*
    * Vector 3 uses no IST stack.
    *
    * For this CPL3-to-CPL0 transition, the CPU pushes
    * five qwords, the stub pushes vector/error code,
    * and the common stub saves fifteen GPRs.
    *
    * Therefore, the complete interrupt frame must begin
    * exactly sizeof(struct interrupt_frame) bytes below
    * the configured TSS.RSP0.
    */
    if (ring3_context.transition_stack_top <
        sizeof(struct interrupt_frame)) {

        return false;
    }

    uintptr_t expected_frame_address =
        ring3_context.transition_stack_top -
        sizeof(struct interrupt_frame);

    if ((uintptr_t)frame != expected_frame_address) {
        return false;
    }

    /*
     * Verify the saved kernel stack pointer.
     *
     * The assembly entry routine pushed six callee-saved
     * registers before recording this address.
     */
    uintptr_t saved_kernel_rsp =
        __atomic_load_n(
            &ring3_saved_kernel_rsp,
            __ATOMIC_ACQUIRE
        );

    uintptr_t stack_base =
        (uintptr_t)thread->stack_base;

    uintptr_t stack_top =
        thread->stack_top;

    if (saved_kernel_rsp == 0 ||
        saved_kernel_rsp < stack_base ||
        saved_kernel_rsp > stack_top ||
        stack_top - saved_kernel_rsp <
            RING3_SAVED_REGISTER_BYTES ||
        (saved_kernel_rsp & 15ULL) != 8ULL) {

        return false;
    }

    /*
     * Verify the active hardware page-table root.
     *
     * The user breakpoint must be executing in the
     * address space owned by this process.
     */
    uint64_t expected_root = 0;

    if (!vmm_address_space_get_root_physical(
            ring3_context.address_space,
            &expected_root
        )) {

        return false;
    }

    if (ring3_read_cr3_root() != expected_root) {
        return false;
    }

    /*
     * All validation succeeded.
     *
     * Redirect IRETQ to a privileged assembly trampoline.
     *
     * IMPORTANT:
     *
     * The destination CS is changed to Ring 0.
     * Consequently, IRETQ consumes only RIP, CS and RFLAGS.
     * The saved user RSP and SS remain on the transition
     * stack and are discarded when the trampoline switches
     * back to the saved worker kernel stack.
     */
    extern void arch_ring3_return_trampoline(void);

    /*
    * Redirect IRETQ to the privileged Ring 0 trampoline.
    */
    frame->rip =
        (uint64_t)(uintptr_t)arch_ring3_return_trampoline;

    frame->cs = GDT_KERNEL_CODE_SELECTOR;

    /*
    * IMPORTANT: IRETQ in 64-bit mode always restores
    * RSP and SS, even when returning to the same CPL.
    *
    * We are returning to Ring 0, not to the interrupted
    * user stack.
    *
    * Therefore, construct a new kernel return stack:
    *
    *     RSP = dedicated transition stack top
    *     SS  = kernel data selector
    *
    * The transition stack is already mapped in the shared
    * kernel half of this process's address space.
    *
    * The assembly trampoline will subsequently restore
    * the worker's original suspended kernel stack.
    */
    frame->old_rsp =
        (uint64_t)ring3_context.transition_stack_top;

    frame->old_ss = GDT_KERNEL_DATA_SELECTOR;

    /*
    * Return to kernel with interrupts disabled.
    * Do not restore user-controlled RFLAGS.
    */
    frame->rflags = 2ULL;

    /*
     * Consume the one-shot authorization.
     */
    __atomic_store_n(
        &ring3_test_active,
        0,
        __ATOMIC_RELEASE
    );

    __atomic_store_n(
        &ring3_test_hit,
        1,
        __ATOMIC_RELEASE
    );

    return true;
}


/*
 * Confirm a successful breakpoint and trampoline return.
 */
bool ring3_test_was_hit(void) {

    if (__atomic_load_n(
            &ring3_test_hit,
            __ATOMIC_ACQUIRE
        ) != 1) {

        return false;
    }

    /*
     * The assembly trampoline clears this value after
     * restoring the original kernel stack.
     */
    return __atomic_load_n(
        &ring3_saved_kernel_rsp,
        __ATOMIC_ACQUIRE
    ) == 0;
}


/*
 * Clear the controlled test context.
 */
void ring3_test_disarm(void) {

    __atomic_store_n(
        &ring3_test_active,
        0,
        __ATOMIC_RELEASE
    );

    ring3_context.thread = NULL;
    ring3_context.process = NULL;
    ring3_context.address_space = NULL;

    ring3_context.transition_stack_top = 0;

    ring3_context.user_rip = 0;
    ring3_context.user_rsp = 0;
}
