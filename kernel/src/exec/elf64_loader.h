#ifndef HUA_EXEC_ELF64_LOADER_H
#define HUA_EXEC_ELF64_LOADER_H

#include <stdint.h>

#include "exec/elf64.h"
#include "process/process.h"

/*
 * Initial user-process stack layout.
 *
 * 64 KiB mapped RW/NX stack, with one unmapped guard page below it.
 *
 * The top is deliberately below the x86-64 47-bit user address limit.
 */
#define ELF64_USER_STACK_SIZE          (64ULL * 1024ULL)
#define ELF64_USER_STACK_TOP           0x00007FFFFFF00000ULL
#define ELF64_USER_STACK_BASE           \
    (ELF64_USER_STACK_TOP - ELF64_USER_STACK_SIZE)

#define ELF64_USER_STACK_GUARD          \
    (ELF64_USER_STACK_BASE - ELF64_PAGE_SIZE)

#define ELF64_USER_STACK_PAGE_COUNT     \
    (ELF64_USER_STACK_SIZE / ELF64_PAGE_SIZE)

/*
 * Initial stack words:
 *
 *   argc
 *   argv[0] = NULL
 *   envp[0] = NULL
 *   AT_NULL
 *   AT_NULL value
 *   alignment padding
 *
 * Six 64-bit words preserve 16-byte stack alignment.
 */
#define ELF64_USER_INITIAL_STACK_SIZE   48ULL

/*
 * Results from the process-integrated ELF64 loader.
 *
 * Parser and preparation details are optionally returned through
 * elf_status_out. This enum describes the higher-level operation.
 */
typedef enum {

    ELF64_LOADER_STATUS_OK = 0,

    ELF64_LOADER_STATUS_INVALID_ARGUMENT,

    ELF64_LOADER_STATUS_PARSE_FAILED,

    ELF64_LOADER_STATUS_OUT_OF_MEMORY,

    ELF64_LOADER_STATUS_PROCESS_STATE,

    ELF64_LOADER_STATUS_ADDRESS_CONFLICT,

    ELF64_LOADER_STATUS_ADDRESS_SPACE_ERROR,

    ELF64_LOADER_STATUS_PREPARATION_FAILED,

    ELF64_LOADER_STATUS_OWNERSHIP_FAILED,

    ELF64_LOADER_STATUS_MAPPING_FAILED,

    ELF64_LOADER_STATUS_CLEANUP_FAILED,

    ELF64_LOADER_STATUS_THREAD_CREATE_FAILED,

    ELF64_LOADER_STATUS_SCHEDULER_FAILED

} elf64_loader_status_t;

/*
 * Load an ELF64 executable into an existing NEW user process.
 *
 * The caller must:
 *     - Own a reference to process for the entire operation.
 *     - Keep file_data valid and unchanged until this function returns.
 *     - Supply a writable entry_out that does not overlap file_data.
 *
 * The process must be eligible for process_image_load_begin().
 *
 * Success:
 *     Returns ELF64_LOADER_STATUS_OK.
 *     *entry_out receives the executable entry address.
 *     The process owns all image frames and mappings.
 *     The process remains NEW; no thread is created.
 *
 * Failure:
 *     *entry_out remains unchanged.
 *     Any image mappings and frames are rolled back when possible.
 *
 * Address conflict:
 *     Existing mappings are detected before physical image pages
 *     are allocated. The process image-load reservation is ended,
 *     and the process remains eligible for a later load attempt.
 *
 * Cleanup failure:
 *     Returns ELF64_LOADER_STATUS_CLEANUP_FAILED.
 *     If ownership had transferred to the process, the remaining
 *     page records stay attached for the process cleanup path to retry.
 *
 * elf_status_out may be NULL. When supplied, it receives the most
 * recent ELF parser/preparation status. It is ELF64_STATUS_OK for
 * failures unrelated to parsing or physical-page preparation.
 */
elf64_loader_status_t elf64_load_process(
    process_t *process,
    const void *file_data,
    uint64_t file_size,
    uint64_t *entry_out,
    elf64_status_t *elf_status_out
);

/*
 * Construct the initial user stack for an already loaded NEW
 * user process.
 *
 * The ELF image must have been successfully committed first.
 *
 * Success:
 *     The process owns every stack frame and mapping.
 *     The initial RSP is returned in user_rsp_out.
 *     The process is ready for its first thread to be attached.
 *
 * Failure:
 *     The output remains unchanged.
 *     Successfully allocated pages are released through the
 *     process-owned cleanup path. Cleanup failures remain owned
 *     by the process for later retry.
 */
elf64_loader_status_t elf64_prepare_user_stack(
    process_t *process,
    uint64_t *user_rsp_out
);

/*
 * Convenience operation: load an ELF image and then prepare
 * its initial stack.
 *
 * The caller retains the process reference.
 *
 * Outputs are published only after both operations succeed.
 * If stack preparation fails after ELF loading succeeds, the
 * ELF image remains committed. The caller may retry stack
 * preparation with elf64_prepare_user_stack(), or destroy the
 * process.
 */
elf64_loader_status_t elf64_load_process_with_stack(
    process_t *process,
    const void *file_data,
    uint64_t file_size,
    uint64_t *entry_out,
    uint64_t *user_rsp_out,
    elf64_status_t *elf_status_out
);

/*
 * Create and enqueue the first scheduler-managed user thread.
 *
 * Requires a committed ELF image and a committed initial stack.
 * Success means the process has been made runnable.
 *
 * The function never returns a thread pointer: after enqueue,
 * the TCB may execute and be reaped immediately.
 */
elf64_loader_status_t elf64_start_process(
    process_t *process,
    uint64_t user_entry,
    uint64_t user_rsp
);

/*
 * Return a readable description of a loader result.
 */
const char *elf64_loader_status_string(
    elf64_loader_status_t status
);

#endif
