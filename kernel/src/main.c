#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

#include "drivers/console/font.h"
#include "drivers/console/console.h"
#include "drivers/input/keyboard.h"
#include "shell/shell.h"
#include <stddef.h>
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "interrupts/irq.h"
#include "firmware/acpi/acpi.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/tsc.h"
#include "arch/x86_64/hpet.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/ioapic.h"
#include "time/time.h"
#include "time/clocksource.h"
#include "time/clockevent.h"
#include "time/timer_queue.h"
#include "scheduler/scheduler.h"
#include "process/process.h"
#include "arch/x86_64/syscall.h"

#include "tests/process_tests.h"
#include "tests/time_tests.h"
#include "tests/clockevent_tests.h"
#include "tests/timer_queue_tests.h"
#include "tests/keyboard_tests.h"
#include "tests/ioapic_tests.h"
#include "tests/lapic_tests.h"
#include "tests/tsc_tests.h"
#include "tests/hpet_tests.h"
#include "tests/heap_tests.h"
#include "tests/pmm_tests.h"
#include "tests/vmm_tests.h"
#include "tests/acpi_tests.h"
#include "tests/thread_tests.h"
#include "tests/scheduler_tests.h"
#include "tests/scheduler_integration_tests.h"
#include "tests/scheduler_preemption_tests.h"
#include "tests/wait_queue_tests.h"
#include "tests/sleep_queue_tests.h"
#include "tests/scheduler_sleep_tests.h"
#include "tests/spinlock_tests.h"
#include "tests/mutex_tests.h"
#include "tests/semaphore_tests.h"
#include "tests/condition_tests.h"
#include "tests/condition_integration_tests.h"
#include "tests/rwlock_tests.h"
#include "tests/rwlock_integration_tests.h"
#include "tests/raw_spinlock_tests.h"
#include "tests/address_space_tests.h"
#include "tests/gdt_tests.h"
#include "tests/ring3_tests.h"
#include "tests/msr_tests.h"
#include "tests/syscall_tests.h"
#include "tests/elf64_tests.h"
#include "tests/elf64_loader_tests.h"

#include "diagnostics/memory_diag.h"

extern char __text_start[];
extern char __text_end[];

extern char __rodata_start[];
extern char __rodata_end[];

extern char __data_start[];
extern char __data_end[];

extern char __bss_start[];
extern char __bss_end[];

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] =
    LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memory_map_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .revision = 0,
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

static struct font hua_font;

static void hcf(void) {
    for (;;) {
        asm volatile ("hlt");
    }
}

/*
 * HUA boot output configuration.
 *
 * 0 = clean normal startup
 * 1 = detailed developer diagnostics
 */
#ifndef HUA_BOOT_VERBOSE
#define HUA_BOOT_VERBOSE 0
#endif

static void console_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";

    console_write("0x");

    for (int shift = 60; shift >= 0; shift -= 4) {
        console_put_char(digits[(value >> shift) & 0xF]);
    }
}

static void boot_verbose_write(const char *text) {
#if HUA_BOOT_VERBOSE
    console_write(text);
#else
    (void)text;
#endif
}

static void boot_verbose_write_hex(uint64_t value) {
#if HUA_BOOT_VERBOSE
    console_write_hex(value);
#else
    (void)value;
#endif
}

static void boot_verbose_put_char(char character) {
#if HUA_BOOT_VERBOSE
    console_put_char(character);
#else
    (void)character;
#endif
}

void kmain(void) {

    // Keep interrupts disabled until HUA's interrupt subsystem is ready.
    asm volatile ("cli" ::: "memory");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        hcf();
    }

    if (module_request.response == 0 ||
        module_request.response->module_count < 1 ||
        module_request.response->modules == 0) {
        hcf();
    }

    struct limine_file *font_file =
        module_request.response->modules[0];

    if (!font_init(&hua_font, font_file)) {
        hcf();
    }

    if (framebuffer_request.response == 0 ||
        framebuffer_request.response->framebuffer_count < 1 ||
        framebuffer_request.response->framebuffers == 0) {
        hcf();
    }

    bool console_ready = false;

    for (uint64_t i = 0;
         i < framebuffer_request.response->framebuffer_count;
         i++) {

        struct limine_framebuffer *fb =
            framebuffer_request.response->framebuffers[i];

        if (fb == 0 ||
            fb->memory_model != LIMINE_FRAMEBUFFER_RGB ||
            fb->bpp != 32) {
            continue;
        }

        if (!console_init(fb, &hua_font)) {
            continue;
        }

        console_ready = true;
        /*
        * Draw the HUA startup wordmark.
        *
        * Scale 6 produces 48x96 characters using the
        * existing 8x16 Terminus font.
        */
        uint64_t logo_scale = 6;
        uint64_t logo_width = 3 * 8 * logo_scale;
        uint64_t logo_x = 0;

        if (fb->width > logo_width) {
            logo_x = (fb->width - logo_width) / 2;
        }

        font_draw_text_scaled(
            fb,
            &hua_font,
            logo_x,
            32,
            "HUA",
            logo_scale,
            0x36BFFF,
            0x101820
        );

        /*
        * Leave enough terminal rows below the logo
        * for the startup output.
        */
        console_write("\n\n\n\n\n\n\n");
        console_set_color(0xFFFFFF, 0x101820);
        console_write("HUA OS  |  version 0.1.0\n\n");

        console_set_color(0x36D399, 0x101820);
        console_write("[ OK ] Framebuffer initialized\n");
        console_write("[ OK ] Font initialized\n");
        console_write("[ OK ] Console initialized\n");
        console_write("\n");

        break;
    }

    if (!console_ready) {
        hcf();
    }

    // Initialize HUA's own GDT and Task State Segment.
    console_write("\nInitializing core architecture...\n");

    if (!gdt_init()) {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] GDT/TSS initialization\n");
        console_set_color(0xFFFFFF, 0x101820);
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] GDT and TSS initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Verify GDT descriptors and TSS stack configuration.
    */
    console_write("\nRunning GDT and TSS self-tests...\n");

    if (!gdt_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] GDT and TSS tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] GDT descriptor validation\n");
    boot_verbose_write("[ OK ] Ring 3 descriptor validation\n");
    boot_verbose_write("[ OK ] TSS kernel-stack validation\n");
    boot_verbose_write("[ OK ] TSS stack update and restoration\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Verify the common x86-64 MSR access foundation.
    *
    * Long mode is already active, so IA32_EFER must
    * report both LME and LMA.
    */
    console_write("\nRunning CPU MSR access tests...\n");

    if (!msr_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] CPU MSR access tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] CPU MSR access tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Verify CPU syscall support and HUA's syscall ABI.
    *
    * This is capability detection only. SYSCALL is
    * not enabled at this stage.
    */
    console_write("\nRunning syscall capability and ABI tests...\n");

    if (!syscall_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] SYSCALL capability and ABI tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] SYSCALL capability and ABI tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Initialize trusted syscall-entry stack state.
    *
    * At this point the GDT and TSS have already been
    * initialized, so TSS.RSP0 is available.
    *
    * This does not enable SYSCALL.
    */
    console_write("\nInitializing trusted syscall-entry state...\n");

    if (!arch_syscall_state_init()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Trusted syscall-entry state\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Trusted syscall-entry state initialized\n");
    console_set_color(0xFFFFFF, 0x101820);


    /*
    * Verify trusted syscall-entry state.
    */
    console_write("Running syscall-entry state tests...\n");

    if (!syscall_state_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Syscall-entry state tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Syscall-entry state tests passed\n");

    /*
    * Configure the x86-64 SYSCALL/SYSRET MSRs.
    *
    * Trusted stack state and GDT/TSS initialization
    * must already be complete.
    */
    console_write(
        "\nConfiguring SYSCALL/SYSRET MSRs...\n"
    );

    if (!arch_syscall_enable()) {

        console_set_color(0xFF5555, 0x101820);

        console_write(
            "[ FAILED ] SYSCALL/SYSRET MSR configuration\n"
        );

        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);

    console_write(
        "[ OK ] SYSCALL/SYSRET MSRs configured and verified\n"
    );

    console_set_color(0xFFFFFF, 0x101820);

    // Initialize HUA's Interrupt Descriptor Table.
    console_write("Initializing interrupt subsystem...\n");

    if (!idt_init()) {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] IDT initialization\n");
        console_set_color(0xFFFFFF, 0x101820);
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Interrupt descriptor table initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    // Run IDT entry and return tests.
    console_write("Running IDT self-tests...\n");

    if (!idt_run_tests()) {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] IDT breakpoint test\n");
        console_set_color(0xFFFFFF, 0x101820);
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Breakpoint exception entry and return\n");
    boot_verbose_write("[ OK ] Invalid Opcode exception entry and recovery\n");
    console_set_color(0xFFFFFF, 0x101820);

    // Initialize and test the generic IRQ management core.
    console_write("\nInitializing Generic IRQ Management...\n");

    if (!irq_run_tests()) {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Generic IRQ management tests\n");
        console_set_color(0xFFFFFF, 0x101820);
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] IRQ handler registration\n");
    boot_verbose_write("[ OK ] Duplicate registration protection\n");
    boot_verbose_write("[ OK ] Generic interrupt dispatch\n");
    boot_verbose_write("[ OK ] Per-vector interrupt statistics\n");
    boot_verbose_write("[ OK ] IRQ handler unregistration\n");
    console_set_color(0xFFFFFF, 0x101820);

    // Validate the physical memory map returned by Limine.
    if (memory_map_request.response == 0 ||
        memory_map_request.response->entries == 0 ||
        memory_map_request.response->entry_count == 0) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Physical memory map unavailable\n");

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Physical memory map received\n");

    console_set_color(0xFFFFFF, 0x101820);

    // Validate the Higher Half Direct Map (HHDM).
    if (hhdm_request.response == 0 ||
        hhdm_request.response->offset == 0) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] HHDM unavailable\n");

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] HHDM received\n");

    console_set_color(0xFFFFFF, 0x101820);

    boot_verbose_write("HHDM Offset: ");
    boot_verbose_write_hex(hhdm_request.response->offset);
    boot_verbose_write("\n");

    // Verify the paging mode selected by Limine.
    if (paging_mode_request.response == NULL) {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Paging mode response unavailable\n");
        console_set_color(0xFFFFFF, 0x101820);
        hcf();
    }

    boot_verbose_write("Paging Mode: ");

    if (paging_mode_request.response->mode ==
        LIMINE_PAGING_MODE_X86_64_4LVL) {

        boot_verbose_write("4-Level Paging (x86_64)\n");

    } else if (paging_mode_request.response->mode ==
            LIMINE_PAGING_MODE_X86_64_5LVL) {

        boot_verbose_write("5-Level Paging (x86_64)\n");

    } else {

        console_write("[ FATAL ] Unknown paging mode\n");
        hcf();
    }

    // Read the current CPU paging registers.
    uint64_t cr0;
    uint64_t cr3;
    uint64_t cr4;

    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));

    // Display the register values.
    boot_verbose_write("\nCPU Paging Registers\n");
    boot_verbose_write("====================\n");

    boot_verbose_write("CR0: ");
    boot_verbose_write_hex(cr0);
    boot_verbose_write("\n");

    boot_verbose_write("CR3: ");
    boot_verbose_write_hex(cr3);
    boot_verbose_write("\n");

    boot_verbose_write("CR4: ");
    boot_verbose_write_hex(cr4);
    boot_verbose_write("\n");

    // Verify important paging flags.
    console_write("\nPaging Flags\n");

    if (cr0 & (1ULL << 31)) {
        console_write("[ OK ] CR0.PG - Paging Enabled\n");
    } else {
        console_write("[ FAILED ] CR0.PG - Paging Disabled\n");
        hcf();
    }

    if (cr0 & (1ULL << 16)) {
        console_write("[ OK ] CR0.WP - Write Protection Enabled\n");
    } else {
        console_write("[ FAILED ] CR0.WP - Write Protection Disabled\n");
        hcf();
    }

    if (cr4 & (1ULL << 5)) {
        console_write("[ OK ] CR4.PAE - Physical Address Extension Enabled\n");
    } else {
        console_write("[ FAILED ] CR4.PAE - PAE Disabled\n");
        hcf();
    }

    if (cr4 & (1ULL << 12)) {
        console_write("[ FAILED ] CR4.LA57 - Unexpected 5-Level Paging\n");
        hcf();
    } else {
        console_write("[ OK ] CR4.LA57 - 4-Level Paging Confirmed\n");
    }

    // Detailed memory-map statistics are developer diagnostics.
#if HUA_BOOT_VERBOSE
    if (!memory_diag_print(memory_map_request.response)) {
        console_write("[ FAILED ] Physical memory diagnostics\n");
        hcf();
    }
#endif

    // Initialize the Physical Memory Manager.
    console_set_color(0x36BFFF, 0x101820);
    console_write("\nInitializing Physical Memory Manager...\n");
    console_set_color(0xFFFFFF, 0x101820);

    if (!pmm_init(
            (struct limine_memmap_response *)memory_map_request.response,
            hhdm_request.response->offset)) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Physical Memory Manager initialization\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Physical Memory Manager initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    boot_verbose_write("Total free physical frames: ");
    boot_verbose_write_hex(pmm_get_free_frames());
    boot_verbose_write("\n");

    // Run PMM self-tests.
    if (!pmm_run_tests()) {
        console_write("[ FATAL ] PMM self-tests failed\n");
        hcf();
    }

    /*
    * Test the scheduler-independent raw spinlock.
    *
    * This runs before VMM initialization and before
    * scheduler initialization.
    */
    console_write("\nRunning raw spinlock tests...\n");

    if (!raw_spinlock_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Raw spinlock tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Raw spinlock tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    // Initialize the Virtual Memory Manager.
    console_set_color(0x36BFFF, 0x101820);
    console_write("\nInitializing Virtual Memory Manager...\n");
    console_set_color(0xFFFFFF, 0x101820);

    if (!vmm_init(
            hhdm_request.response->offset,
            memory_map_request.response)) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] VMM initialization\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] VMM initialized\n");

    // Run VMM self-tests.
    if (!vmm_run_tests()) {
        console_write("[ FATAL ] VMM self-tests failed\n");
        hcf();
    }

    // Initialize ACPI using the RSDP supplied by Limine.
    console_write("\nInitializing ACPI...\n");

    boot_verbose_write("RSDP Response Pointer: ");
    boot_verbose_write_hex((uint64_t)(uintptr_t)rsdp_request.response);
    boot_verbose_write("\n");

    if (rsdp_request.response != NULL) {
        boot_verbose_write("RSDP Address: ");
        boot_verbose_write_hex(
            (uint64_t)(uintptr_t)rsdp_request.response->address
        );
        boot_verbose_write("\n");
    }

    if (rsdp_request.response == NULL ||
        rsdp_request.response->address == NULL ||
        !acpi_init(
            rsdp_request.response->address,
            hhdm_request.response->offset
        )) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] ACPI RSDP validation\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    const struct acpi_info *acpi = acpi_get_info();

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] ACPI RSDP signature\n");
    boot_verbose_write("[ OK ] ACPI RSDP checksum\n");

    if (acpi->revision >= 2) {
        boot_verbose_write("[ OK ] ACPI extended checksum\n");
    }

    boot_verbose_write("[ OK ] ACPI root pointer discovered\n");
    console_set_color(0xFFFFFF, 0x101820);

    boot_verbose_write("ACPI Revision: ");
    boot_verbose_write_hex(acpi->revision);
    boot_verbose_write("\n");

    boot_verbose_write("ACPI Root Type: ");

    if (acpi->uses_xsdt) {
        boot_verbose_write("XSDT\n");
    } else {
        boot_verbose_write("RSDT\n");
    }

    boot_verbose_write("ACPI Root Physical Address: ");
    boot_verbose_write_hex(acpi->root_table_address);
    boot_verbose_write("\n");


    /*
     * Display the discovered ACPI System Description Tables.
     */
    boot_verbose_write("\nDiscovering ACPI tables...\n");

    boot_verbose_write("ACPI Table Count: ");
    boot_verbose_write_hex(acpi->table_count);
    boot_verbose_write("\n");

    for (uint32_t i = 0; i < acpi->table_count; i++) {

        const struct acpi_sdt_header *table =
            acpi_get_table(i);

        if (table == NULL) {
            console_write("[ FAILED ] Invalid ACPI table entry\n");
            hcf();
        }

        boot_verbose_write("Table ");
        boot_verbose_write_hex(i);
        boot_verbose_write(": ");

        for (uint32_t j = 0; j < ACPI_SDT_SIGNATURE_SIZE; j++) {
            boot_verbose_put_char(table->signature[j]);
        }

        boot_verbose_write(" | Length: ");
        boot_verbose_write_hex(table->length);
        boot_verbose_write("\n");
    }

    /*
     * Verify that the MADT can be found by signature.
     */
    const struct acpi_sdt_header *madt =
        acpi_find_table("APIC");

    if (madt != NULL && acpi->madt_available) {
        console_set_color(0x36D399, 0x101820);
        console_write("[ OK ] MADT (APIC) discovered\n");
        console_set_color(0xFFFFFF, 0x101820);
    } else {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] MADT (APIC) not found\n");
        console_set_color(0xFFFFFF, 0x101820);
    }


    /*
     * Parse the MADT hardware descriptions.
     */
    console_write("\nParsing MADT hardware information...\n");

    if (madt == NULL || !acpi_madt_parse()) {
        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] MADT parsing\n");
        console_set_color(0xFFFFFF, 0x101820);
        hcf();
    }

    const struct acpi_madt_info *madt_info =
        acpi_madt_get_info();

    if (madt_info == NULL) {
        console_write("[ FATAL ] MADT information unavailable\n");
        hcf();
    }

    /*
     * Run isolated ACPI/MADT parser tests.
     *
     * These tests use synthetic table buffers and do not
     * modify the live MADT information.
     */
    if (!acpi_run_tests()) {
        console_write("[ FATAL ] ACPI/MADT self-tests failed\n");
        hcf();
    }


    /*
     * Initialize HPET as HUA's hardware clocksource.
     *
     * ACPI and VMM are already initialized.
     * HPET does not require LAPIC or I/O APIC initialization.
     */
    console_write("\nInitializing HPET...\n");

    hpet_status_t hpet_status = hpet_init();

    if (hpet_status != HPET_STATUS_OK) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] HPET initialization\n");
        console_write("HPET status: ");
        console_write_hex(hpet_status);
        console_write("\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    const struct hpet_info *hpet_info =
        hpet_get_info();

    if (hpet_info == NULL) {
        console_write("[ FATAL ] Cannot retrieve HPET information\n");
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] HPET initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    boot_verbose_write("HPET Physical Address: ");
    boot_verbose_write_hex(hpet_info->physical_address);
    boot_verbose_write("\n");

    boot_verbose_write("HPET Vendor ID: ");
    boot_verbose_write_hex(hpet_info->vendor_id);
    boot_verbose_write("\n");

    boot_verbose_write("HPET Number: ");
    boot_verbose_write_hex(hpet_info->hpet_number);
    boot_verbose_write("\n");

    boot_verbose_write("HPET Counter Width: ");
    boot_verbose_write(
        hpet_info->counter_64_bit
            ? "64-bit\n"
            : "32-bit\n"
    );

    boot_verbose_write("HPET Counter Period (fs): ");
    boot_verbose_write_hex(hpet_info->period_femtoseconds);
    boot_verbose_write("\n");

    boot_verbose_write("HPET Frequency (Hz): ");
    boot_verbose_write_hex(hpet_info->frequency_hz);
    boot_verbose_write("\n");

    boot_verbose_write("HPET Initial Counter: ");
    boot_verbose_write_hex(hpet_info->initial_counter);
    boot_verbose_write("\n");

    /*
     * Run HPET hardware tests.
     */
    if (!hpet_run_tests()) {
        console_write("[ FATAL ] HPET self-tests failed\n");
        hcf();
    }

    /*
    * Initialize generic kernel timekeeping.
    *
    * HPET is the hardware clocksource.
    * LAPIC timer calibration will be implemented separately.
    */
    console_write("\nInitializing kernel timekeeping...\n");

    time_status_t time_status = time_init();

    if (time_status != TIME_STATUS_OK) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Kernel timekeeping initialization\n");

        console_write("Time status: ");
        console_write_hex(time_status);
        console_write("\n");

        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Kernel timekeeping initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    const struct clocksource_info *clock_info =
        clocksource_get_info();

    if (clock_info != NULL) {

        boot_verbose_write("Clocksource: ");
        boot_verbose_write(clock_info->name);
        boot_verbose_write("\n");

        boot_verbose_write("Clocksource frequency: ");
        boot_verbose_write_hex(clock_info->frequency_hz);
        boot_verbose_write(" Hz\n");
    }

    /*
    * Run generic timekeeping tests.
    */
    if (!time_run_tests()) {
        console_write("[ FATAL ] Timekeeping self-tests failed\n");
        hcf();
    }

    /*
     * Initialize CPU timestamp counter support.
     *
     * This is independent of Local APIC initialization.
     * Lack of TSC support does not prevent the kernel
     * from booting; a different clocksource may be used.
     */
    console_write("\nInitializing CPU TSC...\n");

    tsc_status_t tsc_status = tsc_init();

    if (tsc_status == TSC_STATUS_OK) {

        struct tsc_info tsc_info = {0};

        if (!tsc_get_info(&tsc_info)) {
            console_write("[ FATAL ] Cannot retrieve TSC information\n");
            hcf();
        }

        console_set_color(0x36D399, 0x101820);
        console_write("[ OK ] CPU TSC supported\n");
        console_set_color(0xFFFFFF, 0x101820);

        boot_verbose_write("Invariant TSC: ");
        boot_verbose_write(
            tsc_info.invariant ? "YES\n" : "NO\n"
        );

        boot_verbose_write("TSC Frequency Available: ");
        boot_verbose_write(
            tsc_info.frequency_available ? "YES\n" : "NO\n"
        );

        if (tsc_info.frequency_available) {
            boot_verbose_write("CPUID TSC Frequency (Hz): ");
            boot_verbose_write_hex(tsc_info.frequency_hz);
            boot_verbose_write("\n");
        }

    } else if (tsc_status == TSC_STATUS_UNSUPPORTED) {

        console_write("[ WARN ] CPU does not report TSC support\n");

    } else {

        console_write("[ FATAL ] TSC initialization failed\n");
        hcf();
    }

    if (!tsc_run_tests()) {
        console_write("[ FATAL ] TSC tests failed\n");
        hcf();
    }


    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] MADT parsed successfully\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
     * Local APIC information.
     */
    boot_verbose_write("\nLocal APIC Physical Address: ");
    boot_verbose_write_hex(madt_info->local_apic_address);
    boot_verbose_write("\n");

    boot_verbose_write("MADT Flags: ");
    boot_verbose_write_hex(madt_info->flags);
    boot_verbose_write("\n");

    /*
     * Processor information.
     */
    boot_verbose_write("\nDiscovered Processors: ");
    boot_verbose_write_hex(madt_info->processor_count);
    boot_verbose_write("\n");

    for (uint32_t i = 0; i < madt_info->processor_count; i++) {

        const struct acpi_madt_processor *processor =
            &madt_info->processors[i];

        boot_verbose_write("Processor ");
        boot_verbose_write_hex(i);
        boot_verbose_write(" | UID: ");
        boot_verbose_write_hex(processor->processor_uid);

        boot_verbose_write(" | APIC ID: ");
        boot_verbose_write_hex(processor->apic_id);

        boot_verbose_write(" | Flags: ");
        boot_verbose_write_hex(processor->flags);

        boot_verbose_write(" | Type: ");
        boot_verbose_write(
            processor->x2apic ? "x2APIC\n" : "Local APIC\n"
        );

        if (processor->flags & 1U) {
            boot_verbose_write("  [ OK ] Processor enabled\n");
        } else if (processor->flags & 2U) {
            boot_verbose_write("  [ INFO ] Processor online-capable\n");
        } else {
            boot_verbose_write("  [ INFO ] Processor disabled\n");
        }
    }

    /*
     * I/O APIC information.
     */
    boot_verbose_write("\nDiscovered I/O APICs: ");
    boot_verbose_write_hex(madt_info->io_apic_count);
    boot_verbose_write("\n");

    for (uint32_t i = 0; i < madt_info->io_apic_count; i++) {

        const struct acpi_madt_io_apic *io_apic =
            &madt_info->io_apics[i];

        boot_verbose_write("I/O APIC ");
        boot_verbose_write_hex(i);

        boot_verbose_write(" | ID: ");
        boot_verbose_write_hex(io_apic->apic_id);

        boot_verbose_write(" | MMIO: ");
        boot_verbose_write_hex(io_apic->physical_address);

        boot_verbose_write(" | Global IRQ Base: ");
        boot_verbose_write_hex(io_apic->global_irq_base);

        boot_verbose_write("\n");
    }

    /*
     * Interrupt Source Override information.
     */
    boot_verbose_write("\nInterrupt Source Overrides: ");
    boot_verbose_write_hex(madt_info->iso_count);
    boot_verbose_write("\n");

    for (uint32_t i = 0; i < madt_info->iso_count; i++) {

        const struct acpi_madt_iso *iso =
            &madt_info->interrupt_overrides[i];

        boot_verbose_write("ISO ");
        boot_verbose_write_hex(i);

        boot_verbose_write(" | Bus: ");
        boot_verbose_write_hex(iso->bus);

        boot_verbose_write(" | Source IRQ: ");
        boot_verbose_write_hex(iso->source);

        boot_verbose_write(" | Global IRQ: ");
        boot_verbose_write_hex(iso->global_irq);

        boot_verbose_write(" | Flags: ");
        boot_verbose_write_hex(iso->flags);

        boot_verbose_write("\n");
    }

    /*
     * Local APIC NMI information.
     */
    boot_verbose_write("\nLocal APIC NMI Records: ");
    boot_verbose_write_hex(madt_info->nmi_count);
    boot_verbose_write("\n");

    for (uint32_t i = 0; i < madt_info->nmi_count; i++) {

        const struct acpi_madt_nmi *nmi =
            &madt_info->nmis[i];

        boot_verbose_write("NMI ");
        boot_verbose_write_hex(i);

        boot_verbose_write(" | Processor UID: ");
        boot_verbose_write_hex(nmi->processor_uid);

        boot_verbose_write(" | LINT: ");
        boot_verbose_write_hex(nmi->lint);

        boot_verbose_write(" | Flags: ");
        boot_verbose_write_hex(nmi->flags);

        boot_verbose_write(" | Type: ");
        boot_verbose_write(nmi->x2apic ? "x2APIC\n" : "Local APIC\n");
    }

    /*
     * Non-Maskable Interrupt Source information.
     */
    boot_verbose_write("\nNMI Source Records: ");
    boot_verbose_write_hex(madt_info->nmi_source_count);
    boot_verbose_write("\n");

    for (uint32_t i = 0; i < madt_info->nmi_source_count; i++) {

        const struct acpi_madt_nmi_source *nmi_source =
            &madt_info->nmi_sources[i];

        boot_verbose_write("NMI Source ");
        boot_verbose_write_hex(i);

        boot_verbose_write(" | Global IRQ: ");
        boot_verbose_write_hex(nmi_source->global_irq);

        boot_verbose_write(" | Flags: ");
        boot_verbose_write_hex(nmi_source->flags);

        boot_verbose_write("\n");
    }

    /*
    * Mask the legacy 8259 PICs before enabling APIC routing.
    *
    * MADT flag bit 0 indicates legacy dual-PIC compatibility.
    * Masking is performed regardless so legacy IRQ delivery
    * cannot interfere with HUA's APIC architecture.
    */
    console_write("\nInitializing legacy PIC...\n");

    if (!pic_init((madt_info->flags & 1U) != 0)) {

        console_write("[ FATAL ] Legacy PIC masking failed\n");
        hcf();
    }

    struct pic_info pic_info = {0};

    if (!pic_get_info(&pic_info) ||
        pic_info.master_mask != 0xFF ||
        pic_info.slave_mask != 0xFF) {

        console_write("[ FATAL ] Legacy PIC is not fully masked\n");
        hcf();
    }

    console_write("[ OK ] Legacy PIC initialized and masked\n");

    /*
     * Initialize the Local APIC.
     *
     * ACPI/MADT, VMM, IDT and IRQ core must already
     * be initialized.
     *
     * This does not execute STI.
     */
    console_write("\nInitializing Local APIC...\n");

    lapic_status_t lapic_status =
        lapic_init(madt_info);

    if (lapic_status != LAPIC_STATUS_OK) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Local APIC initialization\n");
        console_write("LAPIC status: ");
        console_write_hex(lapic_status);
        console_write("\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    struct lapic_info lapic_info = {0};

    if (!lapic_get_info(&lapic_info)) {
        console_write("[ FATAL ] Cannot retrieve LAPIC information\n");
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Local APIC initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    boot_verbose_write("LAPIC Mode: ");

    boot_verbose_write(
        lapic_info.x2apic_mode
            ? "x2APIC\n"
            : "xAPIC\n"
    );

    boot_verbose_write("LAPIC ID: ");
    boot_verbose_write_hex(lapic_info.apic_id);
    boot_verbose_write("\n");

    boot_verbose_write("LAPIC Version: ");
    boot_verbose_write_hex(lapic_info.version);
    boot_verbose_write("\n");

    boot_verbose_write("Maximum LVT Entry: ");
    boot_verbose_write_hex(lapic_info.maximum_lvt_entry);
    boot_verbose_write("\n");

    if (!lapic_run_tests()) {
        console_write("[ FATAL ] Local APIC tests failed\n");
        hcf();
    }

    /*
    * Calibrate the LAPIC timer using HPET.
    *
    * Global CPU interrupts are still disabled.
    */
    console_write("\nCalibrating LAPIC clockevent using HPET...\n");

    clockevent_status_t clockevent_status =
        clockevent_init();

    if (clockevent_status != CLOCKEVENT_STATUS_OK) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] LAPIC timer calibration\n");

        console_write("Clockevent status: ");
        console_write_hex(clockevent_status);
        console_write("\n");

        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    struct clockevent_info clockevent_info = {0};

    if (!clockevent_get_info(&clockevent_info)) {
        console_write("[ FATAL ] Cannot retrieve clockevent information\n");
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] LAPIC timer calibrated\n");
    console_set_color(0xFFFFFF, 0x101820);

    boot_verbose_write("Measured LAPIC timer frequency: ");
    boot_verbose_write_hex(clockevent_info.lapic_frequency_hz);
    boot_verbose_write(" Hz\n");

    /*
    * Verify calibration and periodic timer configuration.
    */
    if (!clockevent_run_tests()) {
        console_write("[ FATAL ] Clockevent self-tests failed\n");
        hcf();
    }

    /*
    * Initialize the kernel timer queue.
    *
    * Timekeeping and LAPIC clockevent calibration must
    * already be initialized.
    */
    console_write("\nInitializing kernel timer queue...\n");

    timer_queue_status_t timer_queue_status =
        timer_queue_init();

    if (timer_queue_status != TIMER_QUEUE_STATUS_OK) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Kernel timer queue initialization\n");

        console_write("Timer queue status: ");
        console_write_hex(timer_queue_status);
        console_write("\n");

        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Kernel timer queue initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Verify timer scheduling, expiration, cancellation,
    * periodic timers, ordering, and callback execution.
    *
    * These tests manually poll the queue because normal
    * CPU interrupt delivery has not started yet.
    */
    if (!timer_queue_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Timer queue self-tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Kernel timer queue self-tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Initialize I/O APIC controllers.
    *
    * Requires:
    * - ACPI/MADT discovery.
    * - VMM and IDT initialization.
    * - Generic IRQ core.
    * - Local APIC initialization.
    * - Legacy PIC masking.
    *
    * All I/O APIC redirection entries remain masked.
    */
    console_write("\nInitializing I/O APIC...\n");

    ioapic_status_t ioapic_status =
        ioapic_init(madt_info);

    if (ioapic_status != IOAPIC_STATUS_OK) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] I/O APIC initialization\n");
        console_write("I/O APIC status: ");
        console_write_hex(ioapic_status);
        console_write("\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] I/O APIC initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    boot_verbose_write("I/O APIC controllers: ");
    boot_verbose_write_hex(ioapic_get_controller_count());
    boot_verbose_write("\n");

    for (uint32_t i = 0;
        i < ioapic_get_controller_count();
        i++) {

        struct ioapic_controller_info info = {0};

        if (!ioapic_get_controller_info(i, &info)) {
            console_write("[ FATAL ] Cannot retrieve I/O APIC information\n");
            hcf();
        }

        boot_verbose_write("I/O APIC ID: ");
        boot_verbose_write_hex(info.apic_id);

        boot_verbose_write(" | Version: ");
        boot_verbose_write_hex(info.version);

        boot_verbose_write(" | GSI Base: ");
        boot_verbose_write_hex(info.gsi_base);

        boot_verbose_write(" | Redirection Entries: ");
        boot_verbose_write_hex(info.redirection_count);

        boot_verbose_write("\n");
    }

    /*
    * Run I/O APIC tests.
    *
    * These tests do not unmask any hardware interrupt.
    */
    if (!ioapic_run_tests()) {
        console_write("[ FATAL ] I/O APIC tests failed\n");
        hcf();
    }

    // Initialize the kernel heap.
    console_write("\nInitializing kernel heap...\n");

    if (!heap_init()) {
        console_write("Kernel heap initialization FAILED!\n");
        hcf();
    }

    console_write("Kernel heap initialized successfully.\n");

    // Run heap tests.
    if (!heap_run_tests()) {
        hcf();
    }

    /*
     * Validate ELF64 parsing, page planning, and physical-page
     * preparation.
     *
     * Preparation tests allocate PMM frames and release them.
     * No user address-space mappings are created or modified.
     */
    console_write("\nRunning ELF64 loader foundation tests...\n");

    if (!elf64_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] ELF64 loader foundation tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] ELF64 loader foundation tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Test the address-space foundation.
    *
    * The heap is initialized, so address-space descriptors
    * can be allocated through kmalloc().
    *
    * The raw VMM lock does not require scheduler initialization.
    */
    console_write("\nRunning address-space foundation tests...\n");

    if (!address_space_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Address-space foundation tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    /*
     * Run the comprehensive address-space tests only after
     * heap initialization. Address-space descriptors use kmalloc().
     */
    if (!vmm_address_space_comprehensive_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Comprehensive address-space tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Comprehensive address-space tests passed\n");
    boot_verbose_write("[ OK ] Address-space foundation tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Test thread creation and initial architecture contexts.
    */
    console_write("\nRunning kernel thread foundation tests...\n");

    if (!thread_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Kernel thread foundation tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Kernel thread foundation tests\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Initialize the CPU0 scheduler foundation.
    *
    * This adopts the existing kmain execution as the
    * bootstrap thread and creates the CPU0 idle thread.
    *
    * Scheduling is not started here.
    */
    console_write("\nInitializing kernel scheduler...\n");

    if (!scheduler_init()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Scheduler initialization\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] CPU0 scheduler initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
     * Initialize process management.
     *
     * The heap provides PCB allocation.
     * The scheduler provides the running bootstrap thread
     * required by the process manager's spinlocks.
     */
    console_write("\nInitializing process manager...\n");

    if (!process_init()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Process manager initialization\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Process manager initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
     * Run process manager self-tests.
     */
    console_write("\nRunning process manager tests...\n");

    if (!process_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Process manager tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Process manager tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
     * Test loading a complete ELF image into a process-owned
     * address space.
     *
     * These tests require:
     *     - Heap initialization.
     *     - VMM address-space support.
     *     - Scheduler initialization.
     *     - Process manager initialization.
     */
    console_write(
        "\nRunning process-integrated ELF64 loader tests...\n"
    );

    if (!elf64_loader_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write(
            "[ FAILED ] Process-integrated ELF64 loader tests\n"
        );
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write(
        "[ OK ] Process-integrated ELF64 loader tests passed\n"
    );
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run kernel spinlock tests.
    *
    * Scheduler initialization is required because spinlocks
    * use thread_preempt_disable() and thread_preempt_enable().
    */
    console_write("\nRunning kernel spinlock tests...\n");

    if (!spinlock_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Kernel spinlock tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Kernel spinlock tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run kernel mutex tests.
    *
    * Mutexes depend on the scheduler and wait-queue
    * infrastructure initialized above.
    */
    console_write("\nRunning kernel mutex tests...\n");

    if (!mutex_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Kernel mutex tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Kernel mutex tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run kernel semaphore tests.
    *
    * Semaphores depend on the scheduler,
    * thread blocking, and wait queues.
    */
    console_write("\nRunning kernel semaphore tests...\n");

    if (!semaphore_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Kernel semaphore tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Kernel semaphore tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run kernel condition-variable tests.
    *
    * Condition variables depend on mutexes,
    * wait queues, and scheduler blocking.
    */
    console_write("\nRunning kernel condition-variable tests...\n");

    if (!condition_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FATAL ] Kernel condition-variable tests failed\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Kernel condition-variable tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run scheduler core and run-queue tests.
    */
    console_write("\nRunning scheduler tests...\n");

    if (!scheduler_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Scheduler tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Scheduler tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run wait-queue foundation tests.
    */
    console_write("\nRunning wait queue tests...\n");

    if (!wait_queue_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Wait queue tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Wait queue tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run deadline-ordered sleep-queue tests.
    */
    console_write("\nRunning sleep queue tests...\n");

    if (!sleep_queue_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Sleep queue tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Sleep queue tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run real cooperative scheduling and thread lifecycle tests.
    */
    console_write("\nRunning scheduler integration tests...\n");

    if (!scheduler_integration_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Scheduler integration tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Scheduler integration tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run real condition-variable producer-consumer tests.
    *
    * This verifies mutex release, condition blocking,
    * signaling, mutex reacquisition, and thread cleanup.
    */
    console_write("\nRunning condition-variable integration tests...\n");

    if (!condition_integration_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Condition-variable integration tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Condition-variable integration tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);


    // Running RW lock test
    console_write("\nRunning RW-lock tests...\n");

    if (!rwlock_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[FAIL] RW-lock tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] RW-lock tests\n");
    console_set_color(0xFFFFFF, 0x101820);

    // Running RW lock integration test...
    console_write("\nRunning RW-lock integration tests...\n");

    if (!rwlock_integration_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[FAIL] RW-lock integration tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] RW-lock integration tests\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
     * Initialize the keyboard software state.
     */
    console_write("\nInitializing keyboard driver...\n");

    if (!keyboard_init()) {
        console_write("[ FAILED ] Keyboard software initialization\n");
        hcf();
    }

    /*
     * Run keyboard software tests before activating hardware IRQ1.
     */
    if (!keyboard_run_tests()) {
        console_write("[ FAILED ] Keyboard driver tests\n");
        hcf();
    }

    /*
     * Obtain the current processor's APIC ID.
     */
    struct lapic_info keyboard_lapic_info = {0};

    if (!lapic_get_info(&keyboard_lapic_info) ||
        !keyboard_lapic_info.initialized) {

        console_write("[ FAILED ] Keyboard LAPIC information\n");
        hcf();
    }

    /*
     * Configure PS/2 controller, register IRQ handler,
     * resolve IRQ1 through MADT, and unmask its I/O APIC route.
     */
    keyboard_status_t keyboard_status =
        keyboard_start(keyboard_lapic_info.apic_id);

    if (keyboard_status != KEYBOARD_STATUS_OK) {
        console_write("[ FAILED ] Keyboard interrupt initialization\n");
        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] Interrupt-driven keyboard initialized\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Start the system clockevent at 100 Hz.
    *
    * The timer is now configured and running, but CPU
    * interrupt delivery remains disabled until STI.
    */
    console_write("\nStarting system timer at 100 Hz...\n");

    clockevent_status =
        clockevent_start_periodic(100);

    if (clockevent_status != CLOCKEVENT_STATUS_OK) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] System timer startup\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] System timer started at 100 Hz\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Run real timer-driven scheduler preemption tests.
    *
    * The LAPIC clockevent is running. Enable CPU interrupt
    * delivery so actual timer interrupts can drive scheduling.
    */
    console_write("\nEnabling interrupts for scheduler preemption tests...\n");

    asm volatile ("sti" ::: "memory");

    console_write("\nRunning scheduler preemption tests...\n");

    if (!scheduler_preemption_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Scheduler preemption tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Scheduler preemption tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);

    /*
     * Run real timer-driven thread sleep and wakeup tests.
     */
    console_write("\nRunning scheduler sleep tests...\n");

    if (!scheduler_sleep_run_tests()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("[ FAILED ] Scheduler sleep tests\n");
        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);
    boot_verbose_write("[ OK ] Scheduler sleep tests passed\n");
    console_set_color(0xFFFFFF, 0x101820);


    /*
    * Run the first real Ring 3 privilege-transition test.
    */
    console_write("\nRunning Ring 3 transition tests...\n");

    if (!ring3_run_tests()) {

        console_set_color(0xFF5555, 0x101820);

        console_write(
            "[ FAILED ] Ring 3 transition tests\n"
        );

        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);

    boot_verbose_write(
        "[ OK ] Ring 3 transition tests passed\n"
    );

    console_set_color(0xFFFFFF, 0x101820);

    /*
     * Run real ELF user-process launch tests.
     *
     * This must run after:
     *   - Scheduler initialization.
     *   - SYSCALL initialization.
     *   - Existing controlled Ring 3 transition tests.
     */
    console_write(
        "\nRunning real ELF user-process launch tests...\n"
    );

    if (!elf64_loader_launch_run_tests()) {

        console_set_color(0xFF5555, 0x101820);

        console_write(
            "[ FAILED ] Real ELF user-process launch tests\n"
        );

        console_set_color(0xFFFFFF, 0x101820);

        hcf();
    }

    console_set_color(0x36D399, 0x101820);

    boot_verbose_write(
        "[ OK ] Real ELF user-process launch tests passed\n"
    );

    console_set_color(0xFFFFFF, 0x101820);

    /*
    * Initialize the HUA OS shell.
    */
    shell_init();

    /*
     * Interrupt-driven event loop.
     *
     * CLI + queue check + STI/HLT prevents the lost-wakeup
     * race between checking the queue and sleeping.
     */
    while (1) {

        /*
        * Execute deferred timer callbacks in normal
        * kernel context, never inside an interrupt handler.
        *
        * Limit each pass to 32 callbacks so a large callback
        * backlog cannot indefinitely delay keyboard handling.
        */
        timer_queue_run_deferred(32);

        struct keyboard_event event;

        asm volatile ("cli" ::: "memory");

        if (keyboard_get_event(&event)) {

            asm volatile ("sti" ::: "memory");

            switch (event.type) {

                case KEY_EVENT_CHARACTER:
                    shell_handle_character(event.character);
                    break;

                case KEY_EVENT_UP:
                    console_scroll_up(1);
                    break;

                case KEY_EVENT_DOWN:
                    console_scroll_down(1);
                    break;

                case KEY_EVENT_PAGE_UP:
                    console_page_up();
                    break;

                case KEY_EVENT_PAGE_DOWN:
                    console_page_down();
                    break;
            }

            continue;
        }

        /*
         * STI followed immediately by HLT is an atomic
         * interrupt-enable-and-sleep sequence on x86.
         */
        asm volatile ("sti; hlt" ::: "memory");
    }

}
