/* SPDX-License-Identifier: GPL-2.0-only */
/* BCM215xx GPIO, including the pin levels used by the Wave board setup. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"

#define TYPE_WAVE723_GPIO "wave723-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Wave723Gpio, WAVE723_GPIO)

enum {
    TYPE = 0x00, OUTPUT = 0x10, INPUT = 0x18, PULL_ENABLE = 0x20,
    PULL_UP = 0x28, IRQ_MASK = 0x30, IRQ_CLEAR = 0x38, IRQ_TYPE = 0x40,
    IRQ_STATUS = 0x50, DEBOUNCE = 0x60, END = 0x80,
};

struct Wave723Gpio {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[END / 4];
    uint32_t driven[2], external[2], level[2], pending[2];
};

static unsigned gpio_field(Wave723Gpio *s, unsigned reg, unsigned pin)
{
    return (s->regs[reg / 4 + pin / 16] >> (2 * (pin % 16))) & 3;
}

static void gpio_update(Wave723Gpio *s)
{
    bool irq = false;
    for (unsigned bank = 0; bank < 2; ++bank) {
        uint32_t floating = s->regs[PULL_ENABLE / 4 + bank] & s->regs[PULL_UP / 4 + bank];
        uint32_t levels = (s->external[bank] & s->driven[bank]) | (floating & ~s->driven[bank]);
        for (unsigned bit = 0; bit < 32; ++bit) {
            unsigned pin = bank * 32 + bit;
            unsigned type = gpio_field(s, TYPE, pin);
            uint32_t mask = 1u << bit;
            if (type == 2) {
                levels = (levels & ~mask) | (s->regs[OUTPUT / 4 + bank] & mask);
            } else if (type == 1 && ((s->level[bank] ^ levels) & mask)) {
                unsigned edge = levels & mask ? 1 : 2;
                if (gpio_field(s, IRQ_TYPE, pin) & edge) {
                    s->pending[bank] |= mask;
                }
            }
        }
        s->level[bank] = levels;
        irq |= !!(s->pending[bank] & s->regs[IRQ_MASK / 4 + bank]);
    }
    qemu_set_irq(s->irq, irq);
}

static void gpio_set_input(void *opaque, int pin, int level)
{
    Wave723Gpio *s = opaque;
    unsigned bank = pin / 32;
    uint32_t mask = 1u << (pin % 32);
    s->driven[bank] |= mask;
    s->external[bank] = (s->external[bank] & ~mask) | (level ? mask : 0);
    gpio_update(s);
}

static uint64_t gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723Gpio *s = opaque;
    switch (offset) {
    case INPUT: case INPUT + 4: return s->level[(offset - INPUT) / 4];
    case IRQ_STATUS: case IRQ_STATUS + 4: return s->pending[(offset - IRQ_STATUS) / 4];
    default: return s->regs[offset / 4];
    }
}

static void gpio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723Gpio *s = opaque;
    switch (offset) {
    case IRQ_CLEAR: case IRQ_CLEAR + 4:
        s->pending[(offset - IRQ_CLEAR) / 4] &= ~value;
        break;
    case INPUT: case INPUT + 4:
    case IRQ_STATUS: case IRQ_STATUS + 4:
    case 0x58: case 0x5c:
        return;
    default:
        s->regs[offset / 4] = value;
    }
    gpio_update(s);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read, .write = gpio_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gpio_init(Object *obj)
{
    Wave723Gpio *s = WAVE723_GPIO(obj);
    memory_region_init_io(&s->mmio, obj, &gpio_ops, s, TYPE_WAVE723_GPIO, END);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(DEVICE(obj), gpio_set_input, 64);
}

static void gpio_reset(DeviceState *dev)
{
    Wave723Gpio *s = WAVE723_GPIO(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->pending, 0, sizeof(s->pending));
    gpio_update(s);
}

static void gpio_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), gpio_reset);
}

static const TypeInfo gpio_info = {
    .name = TYPE_WAVE723_GPIO, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Wave723Gpio), .instance_init = gpio_init,
    .class_init = gpio_class_init,
};

static void wave723_register_gpio(void)
{
    type_register_static(&gpio_info);
}
type_init(wave723_register_gpio)
