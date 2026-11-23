
#ifndef HUA_PIC_H
#define HUA_PIC_H

#include <stdbool.h>
#include <stdint.h>

struct pic_info {
    bool initialized;
    bool legacy_pic_present;
    uint8_t master_mask;
    uint8_t slave_mask;
};

/*
 * Initialize and mask both 8259 PICs.
 *
 * This does not remap vectors or enable interrupts.
 */
bool pic_init(bool legacy_pic_present);

/* Mask every legacy PIC interrupt. */
bool pic_mask_all(void);

/* Mask or unmask an individual legacy IRQ. */
bool pic_mask_irq(uint8_t irq);
bool pic_unmask_irq(uint8_t irq);

/* Retrieve current PIC state. */
bool pic_get_info(struct pic_info *info);

bool pic_is_initialized(void);

#endif
