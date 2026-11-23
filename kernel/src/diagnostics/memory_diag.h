#ifndef MEMORY_DIAG_H
#define MEMORY_DIAG_H

#include <stdbool.h>
#include <limine.h>

bool memory_diag_print(
    const struct limine_memmap_response *memory_map
);

#endif
