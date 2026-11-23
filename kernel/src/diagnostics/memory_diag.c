#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

#include "drivers/console/console.h"
#include "memory_diag.h"


/*
 * Print a 64-bit value in hexadecimal.
 *
 * Kept private to the diagnostics module so it does not
 * depend on main.c's static console helper.
 */
static void memory_diag_write_hex(uint64_t value) {

    const char digits[] = "0123456789ABCDEF";

    console_write("0x");

    for (int i = 60; i >= 0; i -= 4) {

        uint8_t digit = (value >> i) & 0xF;

        console_put_char(digits[digit]);
    }
}


/*
 * Calculate and display physical memory statistics
 * from the memory map provided by Limine.
 *
 * This function does not allocate, free, or modify
 * any physical memory.
 */
bool memory_diag_print(
    const struct limine_memmap_response *memory_map
) {

    if (memory_map == 0 ||
        memory_map->entries == 0 ||
        memory_map->entry_count == 0) {

        return false;
    }

    uint64_t highest_physical_address = 0;
    uint64_t highest_usable_address = 0;
    uint64_t total_usable_memory = 0;
    uint64_t valid_entry_count = 0;

    for (uint64_t i = 0;
         i < memory_map->entry_count;
         i++) {

        const struct limine_memmap_entry *entry =
            memory_map->entries[i];

        if (entry == 0) {
            continue;
        }

        // Reject entries whose address range would overflow.
        if (entry->length > UINT64_MAX - entry->base) {
            continue;
        }

        uint64_t region_end =
            entry->base + entry->length;

        if (region_end > highest_physical_address) {
            highest_physical_address = region_end;
        }

        if (entry->type == LIMINE_MEMMAP_USABLE) {

            if (region_end > highest_usable_address) {
                highest_usable_address = region_end;
            }

            // Protect the accumulated total from overflow.
            if (entry->length <=
                UINT64_MAX - total_usable_memory) {

                total_usable_memory += entry->length;
            }
        }

        valid_entry_count++;
    }

    console_set_color(0x36BFFF, 0x101820);

    console_write("\nPhysical Memory Statistics\n");
    console_write("==========================\n");

    console_set_color(0xFFFFFF, 0x101820);

    console_write("Valid entries: ");
    memory_diag_write_hex(valid_entry_count);

    console_write("\nHighest physical address: ");
    memory_diag_write_hex(highest_physical_address);

    console_write("\nHighest usable address: ");
    memory_diag_write_hex(highest_usable_address);

    console_write("\nTotal usable memory: ");
    memory_diag_write_hex(total_usable_memory);

    console_write("\n");

    return true;
}
