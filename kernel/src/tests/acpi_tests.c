
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "firmware/acpi/acpi.h"
#include "drivers/console/console.h"
#include "acpi_tests.h"

#define ACPI_TEST_BUFFER_SIZE 2048
#define ACPI_TEST_HEADER_SIZE 44

#define ACPI_TEST_LAPIC_ADDRESS 0xFEE00000U
#define ACPI_TEST_IOAPIC_ADDRESS 0xFEC00000U

static uint8_t acpi_test_buffer[ACPI_TEST_BUFFER_SIZE];
static uint32_t acpi_test_length;


/*
 * Test reporting.
 */
static bool acpi_test_failure(const char *message) {

    console_set_color(0xFF5555, 0x101820);
    console_write("[ FAILED ] ");
    console_write(message);
    console_write("\n");
    console_set_color(0xFFFFFF, 0x101820);

    return false;
}


static void acpi_test_success(const char *message) {

    console_set_color(0x36D399, 0x101820);
    console_write("[ OK ] ");
    console_write(message);
    console_write("\n");
    console_set_color(0xFFFFFF, 0x101820);
}


/*
 * Start constructing a synthetic MADT.
 */
static bool acpi_test_begin(void) {

    for (uint32_t i = 0; i < ACPI_TEST_BUFFER_SIZE; i++) {
        acpi_test_buffer[i] = 0;
    }

    acpi_test_buffer[0] = 'A';
    acpi_test_buffer[1] = 'P';
    acpi_test_buffer[2] = 'I';
    acpi_test_buffer[3] = 'C';

    /*
     * Standard ACPI header:
     * Offset 4: Length
     * Offset 8: Revision
     * Offset 9: Checksum
     */
    acpi_test_buffer[8] = 5;

    /*
     * MADT fixed fields:
     * Offset 36: Local APIC address
     * Offset 40: MADT flags
     */
    uint32_t lapic_address = ACPI_TEST_LAPIC_ADDRESS;
    uint32_t madt_flags = 1;

    __builtin_memcpy(
        &acpi_test_buffer[36],
        &lapic_address,
        sizeof(lapic_address)
    );

    __builtin_memcpy(
        &acpi_test_buffer[40],
        &madt_flags,
        sizeof(madt_flags)
    );

    acpi_test_length = ACPI_TEST_HEADER_SIZE;

    return true;
}


/*
 * Append one raw MADT record.
 */
static bool acpi_test_append(
    const uint8_t *record,
    uint32_t length
) {

    if (record == NULL ||
        length == 0 ||
        length > ACPI_TEST_BUFFER_SIZE - acpi_test_length) {
        return false;
    }

    for (uint32_t i = 0; i < length; i++) {
        acpi_test_buffer[acpi_test_length + i] = record[i];
    }

    acpi_test_length += length;

    return true;
}


/*
 * Finalize the synthetic table.
 *
 * The checksum byte is offset 9.
 */
static bool acpi_test_finalize(void) {

    uint32_t length = acpi_test_length;

    __builtin_memcpy(
        &acpi_test_buffer[4],
        &length,
        sizeof(length)
    );

    acpi_test_buffer[9] = 0;

    uint8_t sum = 0;

    for (uint32_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + acpi_test_buffer[i]);
    }

    acpi_test_buffer[9] = (uint8_t)(0U - sum);

    return true;
}


/*
 * Append a processor Local APIC record.
 */
static bool acpi_test_append_processor(
    uint8_t uid,
    uint8_t apic_id,
    uint32_t flags
) {

    uint8_t record[8] = {
        0,
        8,
        uid,
        apic_id,
        0,
        0,
        0,
        0
    };

    __builtin_memcpy(
        &record[4],
        &flags,
        sizeof(flags)
    );

    return acpi_test_append(record, sizeof(record));
}


/*
 * Append an I/O APIC record.
 */
static bool acpi_test_append_io_apic(
    uint8_t apic_id,
    uint32_t address,
    uint32_t global_irq_base
) {

    uint8_t record[12] = {0};

    record[0] = 1;
    record[1] = 12;
    record[2] = apic_id;

    __builtin_memcpy(
        &record[4],
        &address,
        sizeof(address)
    );

    __builtin_memcpy(
        &record[8],
        &global_irq_base,
        sizeof(global_irq_base)
    );

    return acpi_test_append(record, sizeof(record));
}


/*
 * Append an Interrupt Source Override.
 */
static bool acpi_test_append_iso(
    uint8_t bus,
    uint8_t source,
    uint32_t global_irq,
    uint16_t flags
) {

    uint8_t record[10] = {0};

    record[0] = 2;
    record[1] = 10;
    record[2] = bus;
    record[3] = source;

    __builtin_memcpy(
        &record[4],
        &global_irq,
        sizeof(global_irq)
    );

    __builtin_memcpy(
        &record[8],
        &flags,
        sizeof(flags)
    );

    return acpi_test_append(record, sizeof(record));
}


/*
 * Append an NMI Source record.
 */
static bool acpi_test_append_nmi_source(
    uint32_t global_irq,
    uint16_t flags
) {

    uint8_t record[8] = {0};

    record[0] = 3;
    record[1] = 8;

    __builtin_memcpy(
        &record[2],
        &flags,
        sizeof(flags)
    );

    __builtin_memcpy(
        &record[4],
        &global_irq,
        sizeof(global_irq)
    );

    return acpi_test_append(record, sizeof(record));
}


/*
 * Append a Local APIC NMI record.
 */
static bool acpi_test_append_local_nmi(
    uint8_t processor_uid,
    uint16_t flags,
    uint8_t lint
) {

    uint8_t record[6] = {0};

    record[0] = 4;
    record[1] = 6;
    record[2] = processor_uid;
    record[5] = lint;

    __builtin_memcpy(
        &record[3],
        &flags,
        sizeof(flags)
    );

    return acpi_test_append(record, sizeof(record));
}


/*
 * Append a Local APIC Address Override.
 */
static bool acpi_test_append_lapic_override(uint64_t address) {

    uint8_t record[12] = {0};

    record[0] = 5;
    record[1] = 12;

    __builtin_memcpy(
        &record[4],
        &address,
        sizeof(address)
    );

    return acpi_test_append(record, sizeof(record));
}


/*
 * Append an x2APIC processor record.
 */
static bool acpi_test_append_x2apic(
    uint32_t apic_id,
    uint32_t flags,
    uint32_t processor_uid
) {

    uint8_t record[16] = {0};

    record[0] = 9;
    record[1] = 16;

    __builtin_memcpy(
        &record[4],
        &apic_id,
        sizeof(apic_id)
    );

    __builtin_memcpy(
        &record[8],
        &flags,
        sizeof(flags)
    );

    __builtin_memcpy(
        &record[12],
        &processor_uid,
        sizeof(processor_uid)
    );

    return acpi_test_append(record, sizeof(record));
}


/*
 * Append an x2APIC NMI record.
 */
static bool acpi_test_append_x2apic_nmi(
    uint16_t flags,
    uint32_t processor_uid,
    uint8_t lint
) {

    uint8_t record[12] = {0};

    record[0] = 10;
    record[1] = 12;

    __builtin_memcpy(
        &record[2],
        &flags,
        sizeof(flags)
    );

    __builtin_memcpy(
        &record[4],
        &processor_uid,
        sizeof(processor_uid)
    );

    record[8] = lint;

    return acpi_test_append(record, sizeof(record));
}


/*
 * Execute a test expecting either success or failure.
 */
static bool acpi_test_expect(
    const char *name,
    bool expected_success
) {

    struct acpi_madt_info result = {0};

    bool actual_success = acpi_madt_parse_buffer(
        acpi_test_buffer,
        acpi_test_length,
        &result
    );

    if (actual_success != expected_success) {
        return acpi_test_failure(name);
    }

    acpi_test_success(name);

    return true;
}


/*
 * Build a complete table containing all supported record types.
 */
static bool acpi_test_build_complete_table(void) {

    if (!acpi_test_begin()) {
        return false;
    }

    if (!acpi_test_append_processor(1, 2, 1) ||
        !acpi_test_append_io_apic(
            0,
            ACPI_TEST_IOAPIC_ADDRESS,
            0
        ) ||
        !acpi_test_append_iso(0, 1, 1, 0) ||
        !acpi_test_append_nmi_source(9, 0) ||
        !acpi_test_append_local_nmi(0xFF, 0, 1) ||
        !acpi_test_append_lapic_override(0xFEE01000ULL) ||
        !acpi_test_append_x2apic(3, 1, 2) ||
        !acpi_test_append_x2apic_nmi(0, 0xFFFFFFFFU, 0)) {
        return false;
    }

    /*
     * Unknown record type: valid length, safely skipped.
     */
    const uint8_t unknown_record[4] = {
        0x7F, 4, 0xAA, 0x55
    };

    if (!acpi_test_append(
            unknown_record,
            sizeof(unknown_record))) {
        return false;
    }

    return acpi_test_finalize();
}


/*
 * Verify all supported records and their parsed fields.
 */
static bool acpi_test_complete_table(void) {

    if (!acpi_test_build_complete_table()) {
        return acpi_test_failure(
            "Complete MADT fixture construction"
        );
    }

    struct acpi_madt_info result = {0};

    if (!acpi_madt_parse_buffer(
            acpi_test_buffer,
            acpi_test_length,
            &result)) {
        return acpi_test_failure(
            "Complete MADT parsing"
        );
    }

    if (result.processor_count != 2 ||
        result.io_apic_count != 1 ||
        result.iso_count != 1 ||
        result.nmi_source_count != 1 ||
        result.nmi_count != 2 ||
        result.local_apic_address != 0xFEE01000ULL ||
        !result.local_apic_address_overridden ||
        !result.processors[1].x2apic ||
        !result.nmis[1].x2apic ||
        result.nmi_sources[0].global_irq != 9) {
        return acpi_test_failure(
            "Complete MADT parsed fields"
        );
    }

    acpi_test_success(
        "All supported MADT record types and fields"
    );

    return true;
}


/*
 * Test that failed parsing never publishes partial output.
 */
static bool acpi_test_no_partial_result(void) {

    if (!acpi_test_begin()) {
        return false;
    }

    /*
     * Append an invalid Local APIC record length.
     */
    const uint8_t invalid_record[4] = {
        0, 1, 0, 0
    };

    if (!acpi_test_append(
            invalid_record,
            sizeof(invalid_record)) ||
        !acpi_test_finalize()) {
        return false;
    }

    struct acpi_madt_info result = {0};

    result.local_apic_address = 0x12345678ULL;
    result.processor_count = 77;

    if (acpi_madt_parse_buffer(
            acpi_test_buffer,
            acpi_test_length,
            &result)) {
        return acpi_test_failure(
            "Malformed MADT rejection"
        );
    }

    if (result.local_apic_address != 0x12345678ULL ||
        result.processor_count != 77) {
        return acpi_test_failure(
            "MADT failure does not publish partial output"
        );
    }

    acpi_test_success(
        "MADT failure does not publish partial output"
    );

    return true;
}


/*
 * Test header, signature and checksum validation.
 */
static bool acpi_test_header_validation(void) {

    if (!acpi_test_begin() || !acpi_test_finalize()) {
        return false;
    }

    if (!acpi_test_expect(
            "Valid empty MADT",
            true)) {
        return false;
    }

    /*
     * Invalid signature.
     */
    acpi_test_buffer[0] = 'X';
    acpi_test_finalize();

    if (!acpi_test_expect(
            "Invalid MADT signature",
            false)) {
        return false;
    }

    /*
     * Restore a valid table.
     */
    acpi_test_begin();
    acpi_test_finalize();

    /*
     * Invalid checksum.
     */
    acpi_test_buffer[9] ^= 0x01;

    if (!acpi_test_expect(
            "Invalid MADT checksum",
            false)) {
        return false;
    }

    /*
     * Declared length below fixed MADT header.
     */
    acpi_test_begin();
    uint32_t short_length = 43;

    __builtin_memcpy(
        &acpi_test_buffer[4],
        &short_length,
        sizeof(short_length)
    );

    if (!acpi_test_expect(
            "MADT declared length below fixed header",
            false)) {
        return false;
    }

    /*
     * Declared length larger than accessible buffer.
     */
    acpi_test_begin();

    uint32_t excessive_length = ACPI_TEST_BUFFER_SIZE + 1;

    __builtin_memcpy(
        &acpi_test_buffer[4],
        &excessive_length,
        sizeof(excessive_length)
    );

    if (!acpi_test_expect(
            "MADT declared length exceeds buffer",
            false)) {
        return false;
    }

    /*
     * Buffer shorter than the fixed MADT header.
     */
    if (acpi_madt_parse_buffer(
            acpi_test_buffer,
            43,
            &(struct acpi_madt_info){0})) {
        return acpi_test_failure(
            "Short MADT buffer rejection"
        );
    }

    acpi_test_success("Short MADT buffer rejection");

    /*
     * NULL arguments.
     */
    if (acpi_madt_parse_buffer(
            NULL,
            ACPI_TEST_HEADER_SIZE,
            &(struct acpi_madt_info){0}) ||
        acpi_madt_parse_buffer(
            acpi_test_buffer,
            ACPI_TEST_HEADER_SIZE,
            NULL)) {
        return acpi_test_failure(
            "MADT NULL argument rejection"
        );
    }

    acpi_test_success("MADT NULL argument rejection");

    return true;
}


/*
 * Test fixed header address validation.
 */
static bool acpi_test_lapic_address_validation(void) {

    if (!acpi_test_begin()) {
        return false;
    }

    uint32_t address = 0;

    __builtin_memcpy(
        &acpi_test_buffer[36],
        &address,
        sizeof(address)
    );

    acpi_test_finalize();

    if (!acpi_test_expect(
            "Reject zero Local APIC address",
            false)) {
        return false;
    }

    acpi_test_begin();

    address = 0xFEE00001U;

    __builtin_memcpy(
        &acpi_test_buffer[36],
        &address,
        sizeof(address)
    );

    acpi_test_finalize();

    if (!acpi_test_expect(
            "Reject unaligned Local APIC address",
            false)) {
        return false;
    }

    /*
     * Reserved MADT flags must be zero.
     */
    acpi_test_begin();

    uint32_t flags = 2;

    __builtin_memcpy(
        &acpi_test_buffer[40],
        &flags,
        sizeof(flags)
    );

    acpi_test_finalize();

    if (!acpi_test_expect(
            "Reject reserved MADT flags",
            false)) {
        return false;
    }

    acpi_test_success("Local APIC address validation");

    return true;
}


/*
 * Test I/O APIC address validation.
 */
static bool acpi_test_io_apic_validation(void) {

    if (!acpi_test_begin() ||
        !acpi_test_append_io_apic(
            0,
            0,
            0) ||
        !acpi_test_finalize()) {
        return false;
    }

    if (!acpi_test_expect(
            "Reject zero I/O APIC address",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_io_apic(
            0,
            0xFEC00001U,
            0) ||
        !acpi_test_finalize()) {
        return false;
    }

    if (!acpi_test_expect(
            "Reject unaligned I/O APIC address",
            false)) {
        return false;
    }

    /*
     * Duplicate I/O APIC IDs.
     */
    if (!acpi_test_begin() ||
        !acpi_test_append_io_apic(
            0,
            ACPI_TEST_IOAPIC_ADDRESS,
            0) ||
        !acpi_test_append_io_apic(
            0,
            0xFEC01000U,
            24) ||
        !acpi_test_finalize()) {
        return false;
    }

    if (!acpi_test_expect(
            "Reject duplicate I/O APIC ID",
            false)) {
        return false;
    }

    acpi_test_success("I/O APIC address validation");

    return true;
}


/*
 * Test Local APIC Address Override validation.
 */
static bool acpi_test_lapic_override_validation(void) {

    if (!acpi_test_begin() ||
        !acpi_test_append_lapic_override(0) ||
        !acpi_test_finalize()) {
        return false;
    }

    if (!acpi_test_expect(
            "Reject zero Local APIC override",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_lapic_override(0xFEE00001ULL) ||
        !acpi_test_finalize()) {
        return false;
    }

    if (!acpi_test_expect(
            "Reject unaligned Local APIC override",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_lapic_override(0xFEE01000ULL) ||
        !acpi_test_append_lapic_override(0xFEE02000ULL) ||
        !acpi_test_finalize()) {
        return false;
    }

    if (!acpi_test_expect(
            "Reject duplicate Local APIC overrides",
            false)) {
        return false;
    }

    acpi_test_success("Local APIC override validation");

    return true;
}


/*
 * Test record boundaries and fixed record lengths.
 */
static bool acpi_test_record_validation(void) {

    /*
     * Record length smaller than two.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    const uint8_t short_record[2] = {0x7F, 1};

    if (!acpi_test_append(
            short_record,
            sizeof(short_record)) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject MADT record length below two",
            false)) {
        return false;
    }

    /*
     * Record length larger than remaining table bytes.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    const uint8_t truncated_record[4] = {
        0x7F, 12, 0, 0
    };

    if (!acpi_test_append(
            truncated_record,
            sizeof(truncated_record)) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject truncated MADT record",
            false)) {
        return false;
    }

    /*
     * A single trailing byte cannot form a record header.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    const uint8_t trailing_byte = 0x7F;

    if (!acpi_test_append(
            &trailing_byte,
            sizeof(trailing_byte)) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject incomplete MADT record header",
            false)) {
        return false;
    }

    /*
     * Known record with incorrect fixed length.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    const uint8_t wrong_length[4] = {
        0, 4, 0, 0
    };

    if (!acpi_test_append(
            wrong_length,
            sizeof(wrong_length)) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject incorrect known MADT record length",
            false)) {
        return false;
    }

    acpi_test_success("MADT record boundary validation");

    return true;
}


/*
 * Test processor validation.
 */
static bool acpi_test_processor_validation(void) {

    if (!acpi_test_begin() ||
        !acpi_test_append_processor(1, 2, 4) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject reserved processor flags",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_processor(1, 2, 1) ||
        !acpi_test_append_processor(1, 3, 1) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject duplicate processor UID",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_processor(1, 2, 1) ||
        !acpi_test_append_processor(2, 2, 1) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject duplicate processor APIC ID",
            false)) {
        return false;
    }

    acpi_test_success("Processor record validation");

    return true;
}


/*
 * Test interrupt flags and NMI validation.
 */
static bool acpi_test_interrupt_validation(void) {

    if (!acpi_test_begin() ||
        !acpi_test_append_iso(0, 1, 1, 2) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject reserved ISO polarity",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_iso(0, 1, 1, 8) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject reserved ISO flag bits",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_local_nmi(0xFF, 0, 2) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject invalid Local APIC NMI LINT",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_x2apic_nmi(0, 0xFFFFFFFFU, 2) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject invalid x2APIC NMI LINT",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_nmi_source(9, 2) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject invalid NMI Source flags",
            false)) {
        return false;
    }

    if (!acpi_test_begin() ||
        !acpi_test_append_nmi_source(9, 0) ||
        !acpi_test_append_nmi_source(9, 0) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject duplicate NMI Source GSI",
            false)) {
        return false;
    }

    acpi_test_success("Interrupt and NMI record validation");

    return true;
}


/*
 * Test duplicate ISO records.
 */
static bool acpi_test_iso_validation(void) {

    if (!acpi_test_begin() ||
        !acpi_test_append_iso(0, 1, 1, 0) ||
        !acpi_test_append_iso(0, 1, 2, 0) ||
        !acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject duplicate interrupt source override",
            false)) {
        return false;
    }

    acpi_test_success("Interrupt Source Override validation");

    return true;
}


/*
 * Test fixed array capacity limits.
 */
static bool acpi_test_capacity_validation(void) {

    /*
     * Processor capacity.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    for (uint32_t i = 0; i <= ACPI_MADT_MAX_PROCESSORS; i++) {

        if (!acpi_test_append_processor(
                (uint8_t)i,
                (uint8_t)i,
                1)) {
            return false;
        }
    }

    if (!acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject processor capacity overflow",
            false)) {
        return false;
    }

    /*
     * I/O APIC capacity.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    for (uint32_t i = 0; i <= ACPI_MADT_MAX_IO_APICS; i++) {

        if (!acpi_test_append_io_apic(
                (uint8_t)i,
                ACPI_TEST_IOAPIC_ADDRESS + i * 4096U,
                i * 24U)) {
            return false;
        }
    }

    if (!acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject I/O APIC capacity overflow",
            false)) {
        return false;
    }

    /*
     * Interrupt Source Override capacity.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    for (uint32_t i = 0; i <= ACPI_MADT_MAX_ISO; i++) {

        if (!acpi_test_append_iso(
                0,
                (uint8_t)i,
                i,
                0)) {
            return false;
        }
    }

    if (!acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject ISO capacity overflow",
            false)) {
        return false;
    }

    /*
     * NMI capacity.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    for (uint32_t i = 0; i <= ACPI_MADT_MAX_NMI; i++) {

        if (!acpi_test_append_local_nmi(
                (uint8_t)i,
                0,
                0)) {
            return false;
        }
    }

    if (!acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject NMI capacity overflow",
            false)) {
        return false;
    }

    /*
     * NMI Source capacity.
     */
    if (!acpi_test_begin()) {
        return false;
    }

    for (uint32_t i = 0; i <= ACPI_MADT_MAX_NMI_SOURCES; i++) {

        if (!acpi_test_append_nmi_source(i, 0)) {
            return false;
        }
    }

    if (!acpi_test_finalize() ||
        !acpi_test_expect(
            "Reject NMI Source capacity overflow",
            false)) {
        return false;
    }

    acpi_test_success("All MADT fixed-capacity limits");

    return true;
}


/*
 * Complete ACPI/MADT parser test suite.
 */
bool acpi_run_tests(void) {

    console_write("\n");
    console_set_color(0x36BFFF, 0x101820);
    console_write("========== ACPI / MADT TESTS ==========\n");
    console_set_color(0xFFFFFF, 0x101820);

    if (!acpi_test_header_validation() ||
        !acpi_test_lapic_address_validation() ||
        !acpi_test_io_apic_validation() ||
        !acpi_test_lapic_override_validation() ||
        !acpi_test_record_validation() ||
        !acpi_test_processor_validation() ||
        !acpi_test_interrupt_validation() ||
        !acpi_test_iso_validation() ||
        !acpi_test_capacity_validation() ||
        !acpi_test_no_partial_result() ||
        !acpi_test_complete_table()) {

        console_set_color(0xFF5555, 0x101820);
        console_write("ACPI / MADT TESTS FAILED\n");
        console_set_color(0xFFFFFF, 0x101820);

        return false;
    }

    console_set_color(0xFFFFFF, 0x101820);
    console_write("\nALL ACPI / MADT TESTS PASSED\n\n");
    console_set_color(0xFFFFFF, 0x101820);

    return true;
}
