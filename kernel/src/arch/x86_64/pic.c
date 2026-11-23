#include <stddef.h>
#include "pic.h"

#include "arch/x86_64/io.h"

#define PIC_MASTER_COMMAND 0x20
#define PIC_MASTER_DATA    0x21
#define PIC_SLAVE_COMMAND  0xA0
#define PIC_SLAVE_DATA     0xA1

#define PIC_MASK_ALL       0xFF

static struct pic_info pic_state;

bool pic_mask_all(void) {

    x86_outb(PIC_MASTER_DATA, PIC_MASK_ALL);
    x86_outb(PIC_SLAVE_DATA, PIC_MASK_ALL);

    pic_state.master_mask =
        x86_inb(PIC_MASTER_DATA);

    pic_state.slave_mask =
        x86_inb(PIC_SLAVE_DATA);

    return pic_state.master_mask == PIC_MASK_ALL &&
           pic_state.slave_mask == PIC_MASK_ALL;
}

bool pic_init(bool legacy_pic_present) {

    pic_state.initialized = false;
    pic_state.legacy_pic_present = legacy_pic_present;

    /*
     * Mask both controllers before configuring other
     * interrupt routing.
     *
     * We intentionally do not send ICWs or remap vectors.
     * HUA uses the APIC interrupt architecture.
     */
    if (!pic_mask_all()) {
        return false;
    }

    pic_state.initialized = true;

    return true;
}

bool pic_mask_irq(uint8_t irq) {

    if (!pic_state.initialized || irq >= 16) {
        return false;
    }

    if (irq < 8) {

        uint8_t mask = x86_inb(PIC_MASTER_DATA);

        mask |= (uint8_t)(1U << irq);

        x86_outb(PIC_MASTER_DATA, mask);

    } else {

        uint8_t mask = x86_inb(PIC_SLAVE_DATA);

        mask |= (uint8_t)(1U << (irq - 8));

        x86_outb(PIC_SLAVE_DATA, mask);
    }

    return true;
}

bool pic_unmask_irq(uint8_t irq) {

    /*
     * Deliberately refuse to unmask legacy PIC interrupts.
     *
     * HUA's interrupt routing is being moved to the I/O APIC.
     * This API remains reserved for a future explicit
     * compatibility mode.
     */
    (void)irq;

    return false;
}

bool pic_get_info(struct pic_info *info) {

    if (!pic_state.initialized || info == NULL) {
        return false;
    }

    pic_state.master_mask =
        x86_inb(PIC_MASTER_DATA);

    pic_state.slave_mask =
        x86_inb(PIC_SLAVE_DATA);

    *info = pic_state;

    return true;
}

bool pic_is_initialized(void) {
    return pic_state.initialized;
}
