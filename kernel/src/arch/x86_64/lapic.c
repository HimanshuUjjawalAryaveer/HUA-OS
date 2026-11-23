#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "lapic.h"

#include "mm/vmm.h"
#include "interrupts/irq.h"
#include "time/timer_queue.h"

/*
 * HUA's LAPIC virtual mapping reservation.
 *
 * This is separate from the kernel heap arena.
 */
#define LAPIC_VIRTUAL_BASE  0xFFFFFF1000000000ULL

#define LAPIC_PAGE_SIZE     4096ULL
#define LAPIC_PAGE_MASK     (LAPIC_PAGE_SIZE - 1ULL)

/*
 * IA32_TSC_DEADLINE MSR.
 */
#define LAPIC_MSR_TSC_DEADLINE 0x6E0

/*
 * CPUID feature bits.
 */
#define CPUID_APIC             (1U << 9)
#define CPUID_X2APIC           (1U << 21)
#define CPUID_TSC_DEADLINE     (1U << 24)

/*
 * APIC version register.
 */
#define LAPIC_VERSION_MASK     0xFFU
#define LAPIC_MAX_LVT_SHIFT    16

/*
 * PAT memory type: uncacheable.
 */
#define LAPIC_PAT_TYPE_UC      0x00U

/*
 * PAT entry selected by PCD=1, PWT=1, PAT=0.
 */
#define LAPIC_PAT_UC_ENTRY     3U

/*
 * x2APIC MSR range.
 */
#define LAPIC_X2APIC_MSR_BASE  0x800U
#define LAPIC_X2APIC_MSR_LAST  0x8FFU

/*
 * Internal state.
 */
static struct lapic_info lapic_state;

static volatile uint32_t *lapic_mmio;

static bool lapic_tsc_deadline_supported;
static bool lapic_timer_configured;
static lapic_timer_mode_t lapic_timer_mode;
static uint64_t lapic_timer_frequency_hz;

/*
 * CPU feature information.
 */
struct lapic_cpu_features {
    bool apic;
    bool x2apic;
    bool tsc_deadline;
};

static struct lapic_cpu_features lapic_features;


/* -------------------------------------------------------------------------- */
/* CPU and MSR helpers                                                        */
/* -------------------------------------------------------------------------- */

static void lapic_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx
) {
    uint32_t a, b, c, d;

    asm volatile (
        "cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(leaf), "c"(subleaf)
    );

    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}


static uint64_t lapic_rdmsr(uint32_t msr) {

    uint32_t low;
    uint32_t high;

    asm volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );

    return ((uint64_t)high << 32) | low;
}


static void lapic_wrmsr(uint32_t msr, uint64_t value) {

    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    asm volatile (
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
        : "memory"
    );
}


static uint64_t lapic_read_tsc(void) {

    uint32_t low;
    uint32_t high;

    asm volatile (
        "rdtsc"
        : "=a"(low), "=d"(high)
    );

    return ((uint64_t)high << 32) | low;
}


static bool lapic_detect_cpu(void) {

    uint32_t max_leaf;
    uint32_t eax, ebx, ecx, edx;

    lapic_cpuid(
        0,
        0,
        &max_leaf,
        NULL,
        NULL,
        NULL
    );

    if (max_leaf < 1) {
        return false;
    }

    lapic_cpuid(
        1,
        0,
        &eax,
        &ebx,
        &ecx,
        &edx
    );

    lapic_features.apic =
        (edx & CPUID_APIC) != 0;

    lapic_features.x2apic =
        (ecx & CPUID_X2APIC) != 0;

    lapic_features.tsc_deadline =
        (ecx & CPUID_TSC_DEADLINE) != 0;

    lapic_tsc_deadline_supported =
        lapic_features.tsc_deadline;

    return lapic_features.apic;
}


/* -------------------------------------------------------------------------- */
/* Register access                                                            */
/* -------------------------------------------------------------------------- */

static bool lapic_register_valid(uint32_t offset) {

    if ((offset & 0xFULL) != 0) {
        return false;
    }

    switch (offset) {

        case LAPIC_REG_ID:
        case LAPIC_REG_VERSION:
        case LAPIC_REG_TPR:
        case LAPIC_REG_EOI:
        case LAPIC_REG_SVR:
        case LAPIC_REG_ESR:
        case LAPIC_REG_LVT_TIMER:
        case LAPIC_REG_LVT_THERMAL:
        case LAPIC_REG_LVT_PERF:
        case LAPIC_REG_LVT_LINT0:
        case LAPIC_REG_LVT_LINT1:
        case LAPIC_REG_LVT_ERROR:
        case LAPIC_REG_TIMER_INITIAL:
        case LAPIC_REG_TIMER_CURRENT:
        case LAPIC_REG_TIMER_DIVIDE:
            return true;

        default:
            return false;
    }
}


static bool lapic_register_read_internal(
    uint32_t offset,
    uint64_t *value
) {
    if (value == NULL ||
        (!lapic_state.initialized &&
        !lapic_state.globally_enabled)) {

        return false;
    }

    if (!lapic_register_valid(offset)) {
        return false;
    }

    if (lapic_state.x2apic_mode) {

        uint32_t msr =
            LAPIC_X2APIC_MSR_BASE + (offset / 16U);

        if (msr > LAPIC_X2APIC_MSR_LAST) {
            return false;
        }

        *value = lapic_rdmsr(msr);

        return true;
    }

    if (lapic_mmio == NULL) {
        return false;
    }

    /*
     * xAPIC registers occupy 16-byte slots.
     * The actual register value is 32 bits.
     */
    volatile uint32_t *register_address =
        &lapic_mmio[offset / sizeof(uint32_t)];

    *value = *register_address;

    asm volatile ("lfence" ::: "memory");

    return true;
}


static bool lapic_register_write_internal(
    uint32_t offset,
    uint32_t value
) {
    if (!lapic_state.globally_enabled ||
        !lapic_register_valid(offset)) {

        return false;
    }

    if (lapic_state.x2apic_mode) {

        uint32_t msr =
            LAPIC_X2APIC_MSR_BASE + (offset / 16U);

        if (msr > LAPIC_X2APIC_MSR_LAST) {
            return false;
        }

        lapic_wrmsr(msr, value);

        return true;
    }

    if (lapic_mmio == NULL) {
        return false;
    }

    volatile uint32_t *register_address =
        &lapic_mmio[offset / sizeof(uint32_t)];

    *register_address = value;

    asm volatile ("mfence" ::: "memory");

    return true;
}


bool lapic_read_register(
    uint32_t offset,
    uint64_t *value
) {
    if (!lapic_state.initialized) {
        return false;
    }

    return lapic_register_read_internal(offset, value);
}


/* -------------------------------------------------------------------------- */
/* PAT and MMIO                                                               */
/* -------------------------------------------------------------------------- */

static bool lapic_pat_supports_uc(void) {

    uint64_t pat = lapic_rdmsr(LAPIC_MSR_PAT);

    uint8_t entry =
        (uint8_t)(
            pat >> (LAPIC_PAT_UC_ENTRY * 8U)
        );

    return entry == LAPIC_PAT_TYPE_UC;
}


static bool lapic_map_mmio(uint64_t physical_address) {

    if ((physical_address & LAPIC_PAGE_MASK) != 0) {
        return false;
    }

    if (physical_address >
        LAPIC_BASE_ADDRESS_MASK) {

        return false;
    }

    /*
     * PAT entry 3 must select UC when PCD=1 and PWT=1.
     *
     * Do not modify IA32_PAT globally.
     */
    if (!lapic_pat_supports_uc()) {
        return false;
    }

    vmm_mapping_info_t mapping;

    vmm_status_t query =
        vmm_query(LAPIC_VIRTUAL_BASE, &mapping);

    /*
    * A previous initialization attempt may already have
    * created this mapping. Reuse it only if it maps the
    * exact physical LAPIC page with safe permissions.
    */
    if (query == VMM_STATUS_OK) {

        if (mapping.physical_address != physical_address ||
            !mapping.writable ||
            mapping.executable ||
            mapping.user_accessible ||
            mapping.huge_page) {

            return false;
        }

        lapic_mmio =
            (volatile uint32_t *)(uintptr_t)LAPIC_VIRTUAL_BASE;

        lapic_state.physical_address = physical_address;
        lapic_state.virtual_address = LAPIC_VIRTUAL_BASE;

        return true;
    }

    if (query != VMM_STATUS_NOT_MAPPED) {
        return false;
    }

    /*
     * PCD=1 and PWT=1 select PAT entry 3.
     * PAT entry 3 was verified as UC above.
     */
    vmm_status_t status = vmm_map_page(
        LAPIC_VIRTUAL_BASE,
        physical_address,
        VMM_MAP_WRITABLE |
        VMM_MAP_CACHE_DISABLE |
        VMM_MAP_WRITE_THROUGH
    );

    if (status != VMM_STATUS_OK) {
        return false;
    }

    /*
     * Verify the resulting mapping.
     */
    if (vmm_query(LAPIC_VIRTUAL_BASE, &mapping) !=
        VMM_STATUS_OK) {

        uint64_t ignored;

        vmm_unmap_page(
            LAPIC_VIRTUAL_BASE,
            &ignored
        );

        return false;
    }

    const uint64_t required_cache_flags =
        (1ULL << 3) |  /* PWT */
        (1ULL << 4);   /* PCD */

    if (mapping.physical_address != physical_address ||
        !mapping.writable ||
        mapping.executable ||
        mapping.user_accessible ||
        mapping.huge_page ||
        (mapping.flags & required_cache_flags) !=
            required_cache_flags ||
        (mapping.flags & (1ULL << 7)) != 0) {

        return false;
    }

    lapic_mmio =
        (volatile uint32_t *)(uintptr_t)LAPIC_VIRTUAL_BASE;

    lapic_state.physical_address = physical_address;
    lapic_state.virtual_address = LAPIC_VIRTUAL_BASE;

    return true;
}


/* -------------------------------------------------------------------------- */
/* ACPI processor and NMI configuration                                       */
/* -------------------------------------------------------------------------- */

static bool lapic_find_current_processor(
    const struct acpi_madt_info *madt,
    uint32_t apic_id,
    uint32_t *processor_uid,
    bool *x2apic
) {
    if (!madt || !processor_uid || !x2apic) {
        return false;
    }

    for (uint32_t i = 0; i < madt->processor_count; i++) {

        const struct acpi_madt_processor *processor =
            &madt->processors[i];

        if (processor->apic_id != apic_id) {
            continue;
        }

       /*
        * Only processors marked enabled are candidates for
        * the currently executing processor.
        *
        * Online-capable alone does not mean currently enabled.
        */

        if (!(processor->flags & 1U)) {
            continue;
        }

        *processor_uid = processor->processor_uid;
        *x2apic = processor->x2apic;

        return true;
    }

    return false;
}


static bool lapic_nmi_flags_to_lvt(
    uint16_t flags,
    uint32_t *result
) {
    if (result == NULL) {
        return false;
    }

    uint16_t polarity = flags & 3U;
    uint16_t trigger = (flags >> 2) & 3U;

    /*
     * ACPI encoding 2 is reserved for both fields.
     */
    if (polarity == 2U || trigger == 2U) {
        return false;
    }

    /*
     * Conforming defaults:
     * active high, edge triggered.
     */
    uint32_t value = LAPIC_DELIVERY_NMI;

    if (polarity == 3U) {
        value |= LAPIC_LVT_ACTIVE_LOW;
    }

    if (trigger == 3U) {
        value |= LAPIC_LVT_LEVEL_TRIGGER;
    }

    *result = value;

    return true;
}


static bool lapic_configure_lint(
    const struct acpi_madt_info *madt,
    uint32_t apic_id
) {
    uint32_t processor_uid = 0;
    bool processor_x2apic = false;

    bool found_processor =
        lapic_find_current_processor(
            madt,
            apic_id,
            &processor_uid,
            &processor_x2apic
        );

    /*
     * Mask both LINT inputs first.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_LINT0,
            LAPIC_LVT_MASKED) ||
        !lapic_register_write_internal(
            LAPIC_REG_LVT_LINT1,
            LAPIC_LVT_MASKED)) {

        return false;
    }

    if (!found_processor) {
        return true;
    }

    for (uint32_t i = 0; i < madt->nmi_count; i++) {

        const struct acpi_madt_nmi *nmi =
            &madt->nmis[i];

        bool wildcard =
            nmi->x2apic
                ? nmi->processor_uid == 0xFFFFFFFFU
                : nmi->processor_uid == 0xFFU;

        if (!wildcard &&
            (nmi->processor_uid != processor_uid ||
             nmi->x2apic != processor_x2apic)) {

            continue;
        }

        if (nmi->lint > 1U) {
            return false;
        }

        uint32_t offset =
            nmi->lint == 0
                ? LAPIC_REG_LVT_LINT0
                : LAPIC_REG_LVT_LINT1;

        uint32_t value;

        if (!lapic_nmi_flags_to_lvt(
                nmi->flags,
                &value)) {

            return false;
        }

        if (!lapic_register_write_internal(
                offset,
                value)) {

            return false;
        }
    }

    return true;
}


/* -------------------------------------------------------------------------- */
/* Interrupt handlers                                                         */
/* -------------------------------------------------------------------------- */

static void lapic_timer_irq_handler(
    struct interrupt_frame *frame,
    void *context
) {
    (void)frame;
    (void)context;

    /*
     * Record the hardware timer interrupt.
     */
    __atomic_fetch_add(
        &lapic_state.timer_interrupt_count,
        1,
        __ATOMIC_RELAXED
    );

    /*
     * Check the software timer queue.
     *
     * Expired callbacks are placed in the deferred queue.
     * No callback is executed from interrupt context.
     */
    (void)timer_queue_poll();

    /*
     * Notify the Local APIC that this interrupt
     * has been handled.
     */
    lapic_eoi();
}


static void lapic_error_irq_handler(
    struct interrupt_frame *frame,
    void *context
) {
    (void)frame;
    (void)context;

    uint64_t error = 0;

    /*
     * ESR is cleared by writing zero, then read.
     */
    lapic_register_write_internal(
        LAPIC_REG_ESR,
        0
    );

    lapic_register_read_internal(
        LAPIC_REG_ESR,
        &error
    );

    __atomic_store_n(
        &lapic_state.last_error_status,
        (uint32_t)error,
        __ATOMIC_RELAXED
    );

    __atomic_fetch_add(
        &lapic_state.error_interrupt_count,
        1,
        __ATOMIC_RELAXED
    );

    lapic_eoi();
}

/*
 * Best-effort cleanup after a partially completed LAPIC
 * initialization.
 *
 * We deliberately retain the global APIC enablement and
 * any MMIO mapping. This avoids removing the mapping while
 * the hardware APIC is still globally enabled.
 */
static void lapic_cleanup_failed_init(
    bool timer_registered,
    bool error_registered
) {
    /*
     * Stop interrupt sources first.
     */
    lapic_register_write_internal(
        LAPIC_REG_LVT_TIMER,
        LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR
    );

    lapic_register_write_internal(
        LAPIC_REG_LVT_ERROR,
        LAPIC_LVT_MASKED | LAPIC_ERROR_VECTOR
    );

    lapic_register_write_internal(
        LAPIC_REG_LVT_LINT0,
        LAPIC_LVT_MASKED
    );

    lapic_register_write_internal(
        LAPIC_REG_LVT_LINT1,
        LAPIC_LVT_MASKED
    );

    /*
     * Disable LAPIC software interrupt delivery.
     * Preserve the other SVR bits.
     */
    uint64_t svr;

    if (lapic_register_read_internal(
            LAPIC_REG_SVR,
            &svr)) {

        lapic_register_write_internal(
            LAPIC_REG_SVR,
            (uint32_t)svr & ~LAPIC_SVR_SOFTWARE_ENABLE
        );
    }

    /*
     * Remove only handlers successfully registered
     * by this initialization attempt.
     */
    if (error_registered) {
        irq_unregister_handler(LAPIC_ERROR_VECTOR);
    }

    if (timer_registered) {
        irq_unregister_handler(LAPIC_TIMER_VECTOR);
    }

    lapic_state.initialized = false;
    lapic_timer_configured = false;
    lapic_timer_frequency_hz = 0;
}

/* -------------------------------------------------------------------------- */
/* Public initialization                                                      */
/* -------------------------------------------------------------------------- */

lapic_status_t lapic_init(
    const struct acpi_madt_info *madt
) {
    if (lapic_state.initialized) {
        return LAPIC_STATUS_OK;
    }

    if (madt == NULL ||
        madt->local_apic_address == 0 ||
        (madt->local_apic_address & LAPIC_PAGE_MASK)) {

        return LAPIC_STATUS_INVALID_MADT;
    }

    if (!lapic_detect_cpu()) {
        return LAPIC_STATUS_UNSUPPORTED_CPU;
    }

    lapic_state.supported = true;
    lapic_state.x2apic_supported =
        lapic_features.x2apic;

    uint64_t apic_base =
        lapic_rdmsr(LAPIC_MSR_APIC_BASE);

    bool x2apic_mode =
        (apic_base & LAPIC_BASE_X2APIC) != 0;

    bool globally_enabled =
        (apic_base & LAPIC_BASE_GLOBAL_ENABLE) != 0;

    if (x2apic_mode && !lapic_features.x2apic) {
        return LAPIC_STATUS_UNSUPPORTED_MODE;
    }

    /*
     * The architectural x2APIC state requires global APIC
     * enablement as well.
     */
    if (x2apic_mode && !globally_enabled) {
        return LAPIC_STATUS_UNSUPPORTED_MODE;
    }

    /*
     * Enable the APIC globally if firmware left it disabled.
     * Preserve all other IA32_APIC_BASE bits.
     */
    if (!globally_enabled) {

        apic_base |= LAPIC_BASE_GLOBAL_ENABLE;

        lapic_wrmsr(
            LAPIC_MSR_APIC_BASE,
            apic_base
        );

        apic_base =
            lapic_rdmsr(LAPIC_MSR_APIC_BASE);

        if (!(apic_base & LAPIC_BASE_GLOBAL_ENABLE)) {
            return LAPIC_STATUS_INVALID_APIC_BASE;
        }
    }

    lapic_state.globally_enabled = true;
    lapic_state.x2apic_mode = x2apic_mode;

    uint64_t hardware_base =
        apic_base & LAPIC_BASE_ADDRESS_MASK;

    if (x2apic_mode) {

        /*
         * x2APIC register access uses MSRs.
         * No MMIO mapping is required.
         */
        lapic_state.physical_address =
            hardware_base;

    } else {

        /*
         * xAPIC mode uses the firmware-provided MADT address.
         *
         * If the firmware's APIC base differs, update the
         * address field while preserving the control bits.
         */
        if (hardware_base != madt->local_apic_address) {

            if (madt->local_apic_address >
                LAPIC_BASE_ADDRESS_MASK) {

                return LAPIC_STATUS_INVALID_APIC_BASE;
            }

            apic_base =
                (apic_base & ~LAPIC_BASE_ADDRESS_MASK) |
                madt->local_apic_address;

            lapic_wrmsr(
                LAPIC_MSR_APIC_BASE,
                apic_base
            );

            apic_base =
                lapic_rdmsr(LAPIC_MSR_APIC_BASE);

            if ((apic_base & LAPIC_BASE_ADDRESS_MASK) !=
                madt->local_apic_address) {

                return LAPIC_STATUS_INVALID_APIC_BASE;
            }
        }

        if (!lapic_map_mmio(madt->local_apic_address)) {
            return LAPIC_STATUS_MMIO_MAPPING_FAILED;
        }
    }

    /*
     * Read APIC identification and version.
     */
    uint64_t apic_id;
    uint64_t version;

    if (!lapic_register_read_internal(
            LAPIC_REG_ID,
            &apic_id) ||
        !lapic_register_read_internal(
            LAPIC_REG_VERSION,
            &version)) {

        return LAPIC_STATUS_REGISTER_ACCESS_FAILED;
    }

    lapic_state.apic_id =
        x2apic_mode
            ? (uint32_t)apic_id
            : (uint32_t)(apic_id >> 24);

    lapic_state.version =
        (uint32_t)version & LAPIC_VERSION_MASK;

    lapic_state.maximum_lvt_entry =
        ((uint32_t)version >> LAPIC_MAX_LVT_SHIFT) & 0xFFU;

    if (lapic_state.maximum_lvt_entry < 5) {
        return LAPIC_STATUS_UNSUPPORTED_MODE;
    }

    bool timer_registered = false;
    bool error_registered = false;

    lapic_status_t failure_status =
        LAPIC_STATUS_REGISTER_ACCESS_FAILED;

    /*
     * Register handlers before enabling their sources.
     */
    if (!irq_register_handler(
            LAPIC_TIMER_VECTOR,
            lapic_timer_irq_handler,
            NULL)) {

        failure_status = LAPIC_STATUS_IRQ_REGISTRATION_FAILED;
        goto init_failed;
    }

    timer_registered = true;

    if (!irq_register_handler(
            LAPIC_ERROR_VECTOR,
            lapic_error_irq_handler,
            NULL)) {

        failure_status = LAPIC_STATUS_IRQ_REGISTRATION_FAILED;
        goto init_failed;
    }

    error_registered = true;

    /*
     * Mask the timer until explicitly configured and started.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_TIMER,
            LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR)) {

        goto init_failed;
    }

    /*
     * Stop any firmware-configured timer.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_TIMER_INITIAL,
            0)) {

        goto init_failed;
    }

    /*
     * Keep the error source masked during initialization.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_ERROR,
            LAPIC_LVT_MASKED | LAPIC_ERROR_VECTOR)) {

        goto init_failed;
    }

    /*
     * Configure firmware-described LINT NMIs.
     */
    if (!lapic_configure_lint(
            madt,
            lapic_state.apic_id)) {

        goto init_failed;
    }

    /*
     * Set the task priority threshold to zero.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_TPR,
            0)) {

        goto init_failed;
    }

    /*
     * Enable the Local APIC in the Spurious Interrupt
     * Vector Register. Vector 0xFF is reserved for this.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_SVR,
            LAPIC_SPURIOUS_VECTOR |
            LAPIC_SVR_SOFTWARE_ENABLE)) {

        goto init_failed;
    }

    /*
    * Mark the controller ready before enabling its
    * error interrupt source.
    */
    lapic_state.initialized = true;

    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_ERROR,
            LAPIC_ERROR_VECTOR)) {

        goto init_failed;
    }

    return LAPIC_STATUS_OK;


init_failed:

    lapic_cleanup_failed_init(
        timer_registered,
        error_registered
    );

    return failure_status;

}


/* -------------------------------------------------------------------------- */
/* Public information and EOI                                                 */
/* -------------------------------------------------------------------------- */

bool lapic_is_initialized(void) {
    return lapic_state.initialized;
}


bool lapic_get_info(struct lapic_info *info) {

    if (!lapic_state.initialized || info == NULL) {
        return false;
    }

    /*
     * Copy fields that remain stable after LAPIC initialization.
     */
    info->initialized = lapic_state.initialized;
    info->supported = lapic_state.supported;
    info->x2apic_supported = lapic_state.x2apic_supported;
    info->x2apic_mode = lapic_state.x2apic_mode;
    info->globally_enabled = lapic_state.globally_enabled;

    info->apic_id = lapic_state.apic_id;
    info->version = lapic_state.version;
    info->maximum_lvt_entry = lapic_state.maximum_lvt_entry;

    info->physical_address = lapic_state.physical_address;
    info->virtual_address = lapic_state.virtual_address;

    /*
     * Interrupt counters can change asynchronously.
     * Read them atomically.
     */
    info->timer_interrupt_count =
        __atomic_load_n(
            &lapic_state.timer_interrupt_count,
            __ATOMIC_RELAXED
        );

    info->error_interrupt_count =
        __atomic_load_n(
            &lapic_state.error_interrupt_count,
            __ATOMIC_RELAXED
        );

    info->spurious_interrupt_count =
        __atomic_load_n(
            &lapic_state.spurious_interrupt_count,
            __ATOMIC_RELAXED
        );

    info->last_error_status =
        __atomic_load_n(
            &lapic_state.last_error_status,
            __ATOMIC_RELAXED
        );

    return true;
}


bool lapic_eoi(void) {

    if (!lapic_state.initialized) {
        return false;
    }

    return lapic_register_write_internal(
        LAPIC_REG_EOI,
        0
    );
}


bool lapic_handle_spurious_interrupt(void) {

    if (!lapic_state.initialized) {
        return false;
    }

    __atomic_fetch_add(
        &lapic_state.spurious_interrupt_count,
        1,
        __ATOMIC_RELAXED
    );

    return true;
}

static bool lapic_timer_divide_valid(
    lapic_timer_divide_t divide
) {
    switch (divide) {

        case LAPIC_TIMER_DIVIDE_1:
        case LAPIC_TIMER_DIVIDE_2:
        case LAPIC_TIMER_DIVIDE_4:
        case LAPIC_TIMER_DIVIDE_8:
        case LAPIC_TIMER_DIVIDE_16:
        case LAPIC_TIMER_DIVIDE_32:
        case LAPIC_TIMER_DIVIDE_64:
        case LAPIC_TIMER_DIVIDE_128:
            return true;

        default:
            return false;
    }
}

/* -------------------------------------------------------------------------- */
/* Timer                                                                      */
/* -------------------------------------------------------------------------- */

bool lapic_timer_configure(
    lapic_timer_mode_t mode,
    lapic_timer_divide_t divide,
    uint8_t vector
) {
    if (!lapic_state.initialized ||
        vector != LAPIC_TIMER_VECTOR) {

        return false;
    }

    if (mode != LAPIC_TIMER_ONE_SHOT &&
        mode != LAPIC_TIMER_PERIODIC &&
        mode != LAPIC_TIMER_DEADLINE) {

        return false;
    }

    if (mode != LAPIC_TIMER_DEADLINE &&
        !lapic_timer_divide_valid(divide)) {

        return false;
    }

    /*
     * Mask the timer before changing its configuration.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_TIMER,
            LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR)) {

        return false;
    }

    /*
     * Clear an earlier deadline when switching modes.
     */
    if (lapic_timer_configured &&
        lapic_timer_mode == LAPIC_TIMER_DEADLINE) {

        lapic_wrmsr(LAPIC_MSR_TSC_DEADLINE, 0);
    }

    /*
     * Count-based modes use the timer divider.
     * TSC-deadline mode does not.
     */
    if (mode != LAPIC_TIMER_DEADLINE) {

        if (!lapic_register_write_internal(
                LAPIC_REG_TIMER_INITIAL,
                0)) {

            return false;
        }

        if (!lapic_register_write_internal(
                LAPIC_REG_TIMER_DIVIDE,
                (uint32_t)divide)) {

            return false;
        }
    }

    uint32_t lvt =
        LAPIC_TIMER_VECTOR | LAPIC_LVT_MASKED;

    if (mode == LAPIC_TIMER_PERIODIC) {
        lvt |= LAPIC_LVT_PERIODIC;
    } else if (mode == LAPIC_TIMER_DEADLINE) {
        lvt |= LAPIC_LVT_TSC_DEADLINE;
    }

    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_TIMER,
            lvt)) {

        return false;
    }

    lapic_timer_mode = mode;
    lapic_timer_configured = true;

    return true;
}

bool lapic_timer_start(uint32_t initial_count) {

    if (!lapic_state.initialized ||
        !lapic_timer_configured ||
        lapic_timer_mode == LAPIC_TIMER_DEADLINE ||
        initial_count == 0) {

        return false;
    }

    if (!lapic_register_write_internal(
            LAPIC_REG_TIMER_INITIAL,
            initial_count)) {

        return false;
    }

    uint32_t lvt = LAPIC_TIMER_VECTOR;

    if (lapic_timer_mode == LAPIC_TIMER_PERIODIC) {
        lvt |= LAPIC_LVT_PERIODIC;
    }

    return lapic_register_write_internal(
        LAPIC_REG_LVT_TIMER,
        lvt
    );
}

bool lapic_timer_start_deadline(uint64_t deadline_tsc) {

    if (!lapic_state.initialized ||
        !lapic_timer_configured ||
        lapic_timer_mode != LAPIC_TIMER_DEADLINE ||
        !lapic_tsc_deadline_supported ||
        deadline_tsc == 0) {

        return false;
    }

    if (deadline_tsc <= lapic_read_tsc()) {
        return false;
    }

    /*
     * Program the absolute deadline while the timer
     * interrupt is still masked.
     */
    lapic_wrmsr(
        LAPIC_MSR_TSC_DEADLINE,
        deadline_tsc
    );

    /*
     * Now unmask the deadline timer.
     */
    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_TIMER,
            LAPIC_TIMER_VECTOR |
            LAPIC_LVT_TSC_DEADLINE)) {

        lapic_wrmsr(LAPIC_MSR_TSC_DEADLINE, 0);
        return false;
    }

    return true;
}


bool lapic_timer_stop(void) {

    if (!lapic_state.initialized) {
        return false;
    }

    if (!lapic_register_write_internal(
            LAPIC_REG_LVT_TIMER,
            LAPIC_LVT_MASKED |
            LAPIC_TIMER_VECTOR)) {

        return false;
    }

    if (lapic_timer_mode == LAPIC_TIMER_DEADLINE) {

        lapic_wrmsr(
            LAPIC_MSR_TSC_DEADLINE,
            0
        );

    } else {

        if (!lapic_register_write_internal(
                LAPIC_REG_TIMER_INITIAL,
                0)) {

            return false;
        }
    }

    return true;
}


bool lapic_timer_get_current_count(uint32_t *count) {

    if (lapic_timer_mode == LAPIC_TIMER_DEADLINE || !lapic_state.initialized || count == NULL) {
        return false;
    }

    uint64_t value;

    if (!lapic_register_read_internal(
            LAPIC_REG_TIMER_CURRENT,
            &value)) {

        return false;
    }

    *count = (uint32_t)value;

    return true;
}


bool lapic_timer_is_calibrated(void) {
    return lapic_timer_frequency_hz != 0;
}


bool lapic_timer_set_calibrated_frequency(uint64_t frequency_hz) {

    if (!lapic_state.initialized || frequency_hz == 0) {
        return false;
    }

    lapic_timer_frequency_hz = frequency_hz;

    return true;
}


bool lapic_timer_get_calibrated_frequency(uint64_t *frequency_hz) {

    if (!lapic_state.initialized ||
        frequency_hz == NULL ||
        lapic_timer_frequency_hz == 0) {

        return false;
    }

    *frequency_hz = lapic_timer_frequency_hz;

    return true;
}


/* -------------------------------------------------------------------------- */
/* Error diagnostics                                                          */
/* -------------------------------------------------------------------------- */

bool lapic_get_error_status(uint32_t *status) {

    if (!lapic_state.initialized || status == NULL) {
        return false;
    }

    *status = __atomic_load_n(
        &lapic_state.last_error_status,
        __ATOMIC_RELAXED
    );

    return true;
}
