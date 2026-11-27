/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WAVE723_INTC_H
#define WAVE723_INTC_H

#include "hw/core/irq.h"
#include "system/memory.h"

typedef struct Wave723Intc Wave723Intc;
typedef struct Wave723IrqBank {
    Wave723Intc *controller;
    MemoryRegion regs, modem_mask;
    unsigned index;
    uint32_t enable[2], fiq_select[2], trigger[4];
    uint32_t wired, pins, edge, software;
} Wave723IrqBank;

struct Wave723Intc {
    Wave723IrqBank bank[3];
    qemu_irq irq[2], fiq[2];
    qemu_irq rip;
    uint32_t protect, control;
};

void wave723_intc_init(Wave723Intc *ic, Object *owner, MemoryRegion *memory,
                       DeviceState *apps, DeviceState *modem);
void wave723_intc_reset(Wave723Intc *ic);
void wave723_intc_set(void *opaque, int source, int level);
void wave723_intc_pulse(Wave723Intc *ic, unsigned source);

#endif
