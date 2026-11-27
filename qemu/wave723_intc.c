/* SPDX-License-Identifier: GPL-2.0-only */
/* BCM215x shared interrupt sources with separate APPS and WCDMA masks. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/core/qdev.h"
#include "target/arm/cpu-qom.h"
#include "wave723_intc.h"

static unsigned trigger_type(Wave723IrqBank *bank, unsigned pin)
{
    return (bank->trigger[pin / 8] >> (4 * (pin % 8))) & 7;
}

static uint32_t raw_status(Wave723IrqBank *bank)
{
    uint32_t status = bank->software | bank->edge;
    for (uint32_t wired = bank->wired; wired; wired &= wired - 1) {
        unsigned pin = ctz32(wired);
        uint32_t bit = 1U << pin;
        unsigned type = trigger_type(bank, pin);
        if ((type == 0 && (bank->pins & bit)) || (type == 1 && !(bank->pins & bit))) {
            status |= bit;
        }
    }
    return status;
}

static void update(Wave723Intc *ic)
{
    uint32_t irq[2] = {}, fiq[2] = {};
    for (unsigned i = 0; i < ARRAY_SIZE(ic->bank); ++i) {
        Wave723IrqBank *bank = &ic->bank[i];
        uint32_t raw = raw_status(bank);
        for (unsigned cpu = 0; cpu < 2; ++cpu) {
            uint32_t enabled = raw & bank->enable[cpu];
            irq[cpu] |= enabled & ~bank->fiq_select[cpu];
            fiq[cpu] |= enabled & bank->fiq_select[cpu];
        }
    }
    for (unsigned cpu = 0; cpu < 2; ++cpu) {
        qemu_set_irq(ic->irq[cpu], !!irq[cpu]);
        qemu_set_irq(ic->fiq[cpu], !!fiq[cpu]);
    }
}

void wave723_intc_set(void *opaque, int source, int level)
{
    Wave723Intc *ic = opaque;
    Wave723IrqBank *bank = &ic->bank[source / 32];
    unsigned pin = source % 32;
    uint32_t bit = 1U << pin;
    bool was_high = bank->pins & bit;
    unsigned type = trigger_type(bank, pin);
    bank->wired |= bit;
    if (level) {
        bank->pins |= bit;
    } else {
        bank->pins &= ~bit;
    }
    if (was_high != !!level && (type == 6 || type == (level ? 4 : 5))) {
        bank->edge |= bit;
    }
    update(ic);
}

void wave723_intc_pulse(Wave723Intc *ic, unsigned source)
{
    ic->bank[source / 32].edge |= 1U << (source % 32);
    update(ic);
}

/* Bank 2 omits the controller-wide registers present in bank 0. */
static unsigned reg_offset(Wave723IrqBank *bank, hwaddr offset)
{
    if (bank->index == 2 && offset >= 0x20) {
        static const unsigned bank2[] = { 0x28, 0x30, 0x34, 0x2c };
        return bank2[(offset - 0x20) / 4];
    }
    return offset;
}

static uint64_t intc_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723IrqBank *bank = opaque;
    unsigned reg = reg_offset(bank, offset);
    if (reg >= 0x10 && reg < 0x20) return bank->trigger[(reg - 0x10) / 4];
    switch (reg) {
    case 0x00: return bank->enable[0];
    case 0x04: return raw_status(bank);
    case 0x0c: return raw_status(bank) & bank->enable[0];
    case 0x20: return bank->index == 0 ? bank->controller->protect : 0;
    case 0x24: return bank->index == 0 ? bank->controller->control : 0;
    case 0x28: return bank->fiq_select[0];
    case 0x2c: return bank->fiq_select[1];
    case 0x30: return bank->software;
    default: return 0;
    }
}

static void intc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723IrqBank *bank = opaque;
    unsigned reg = reg_offset(bank, offset);
    if (reg >= 0x10 && reg < 0x20) {
        bank->trigger[(reg - 0x10) / 4] = value;
    } else {
        switch (reg) {
        case 0x00: bank->enable[0] = value; break;
        case 0x08: bank->edge &= ~value; break;
        case 0x20: if (!bank->index) bank->controller->protect = value & 1; break;
        case 0x24: if (!bank->index) bank->controller->control = value & 0xffef; break;
        case 0x28: bank->fiq_select[0] = value; break;
        case 0x2c: bank->fiq_select[1] = value; break;
        case 0x30: bank->software |= value; break;
        case 0x34: bank->software &= ~value; break;
        }
    }
    update(bank->controller);
}

static uint64_t modem_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723IrqBank *bank = opaque;
    return offset ? raw_status(bank) & bank->enable[1] : bank->enable[1];
}

static void modem_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723IrqBank *bank = opaque;
    if (!offset) {
        bank->enable[1] = value;
        update(bank->controller);
    }
}

static const MemoryRegionOps intc_ops = {
    .read = intc_read, .write = intc_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps modem_ops = {
    .read = modem_read, .write = modem_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void wave723_intc_init(Wave723Intc *ic, Object *owner, MemoryRegion *memory,
                       DeviceState *apps, DeviceState *modem)
{
    const hwaddr base = 0x08810000;
    const unsigned regs[] = { 0, 0x100, 0x180 }, masks[] = { 0x140, 0x148, 0x1b0 };
    DeviceState *cpus[] = { apps, modem };
    for (unsigned i = 0; i < ARRAY_SIZE(cpus); ++i) {
        ic->irq[i] = qdev_get_gpio_in(cpus[i], ARM_CPU_IRQ);
        ic->fiq[i] = qdev_get_gpio_in(cpus[i], ARM_CPU_FIQ);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ic->bank); ++i) {
        Wave723IrqBank *bank = &ic->bank[i];
        g_autofree char *name = g_strdup_printf("wave723.intc%u", i);
        g_autofree char *mask = g_strdup_printf("wave723.intc%u-modem", i);
        bank->index = i;
        bank->controller = ic;
        memory_region_init_io(&bank->regs, owner, &intc_ops, bank, name, i == 2 ? 0x30 : 0x38);
        memory_region_add_subregion(memory, base + regs[i], &bank->regs);
        memory_region_init_io(&bank->modem_mask, owner, &modem_ops, bank, mask, 8);
        memory_region_add_subregion(memory, base + masks[i], &bank->modem_mask);
    }
}

void wave723_intc_reset(Wave723Intc *ic)
{
    ic->protect = ic->control = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(ic->bank); ++i) {
        Wave723IrqBank *bank = &ic->bank[i];
        memset(bank->enable, 0, sizeof(bank->enable));
        memset(bank->fiq_select, 0, sizeof(bank->fiq_select));
        memset(bank->trigger, 0, sizeof(bank->trigger));
        bank->edge = bank->software = 0;
    }
    update(ic);
}
