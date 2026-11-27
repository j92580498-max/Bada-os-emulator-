/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted from the BCM21553 BSC in assemblycore/qemu-bcm21553 (6cd03d2). */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"

#define TYPE_WAVE723_BSC "wave723-bsc"
OBJECT_DECLARE_SIMPLE_TYPE(Wave723Bsc, WAVE723_BSC)

#define BSC_CS      0x00
#define BSC_DAT     0x08
#define BSC_FCR     0x1c
#define BSC_IER     0x24
#define BSC_ISR     0x28
#define BSC_SFTRST  0x40
#define BSC_CS_EN       0x01
#define BSC_CS_CMDMASK  0x06
#define BSC_CS_ACK      0x08
#define BSC_CS_SCL      0x40
#define BSC_CS_SDA      0x80
#define BSC_ISR_NOACK    0x01
#define BSC_ISR_SES_DONE 0x08

struct Wave723Bsc {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;
    uint8_t regs[0x80];
    bool addr_phase;
    bool in_xfer;
    bool nack;
};

static void bsc_update(Wave723Bsc *s)
{
    qemu_set_irq(s->irq, (s->regs[BSC_ISR] & s->regs[BSC_IER] & 0x0f) != 0);
}

static void bsc_done(Wave723Bsc *s, bool nack)
{
    s->regs[BSC_ISR] |= BSC_ISR_SES_DONE | (nack ? BSC_ISR_NOACK : 0);
}

static void bsc_cmd(Wave723Bsc *s, uint8_t cs)
{
    uint8_t d;

    switch (cs & BSC_CS_CMDMASK) {
    case 0x02:                      /* START / RESTART */
        s->addr_phase = true;
        bsc_done(s, false);
        break;
    case 0x04:                      /* STOP */
        if (s->in_xfer) {
            i2c_end_transfer(s->bus);
            s->in_xfer = false;
        }
        s->addr_phase = false;
        bsc_done(s, false);
        break;
    case 0x06:                      /* READ byte */
        d = s->in_xfer ? i2c_recv(s->bus) : 0xff;
        if (s->in_xfer && (cs & BSC_CS_ACK)) {
            i2c_nack(s->bus);
        }
        s->regs[BSC_DAT] = d;
        bsc_done(s, false);
        break;
    }
}

static void bsc_send(Wave723Bsc *s, uint8_t v)
{
    bool nack;

    if (s->addr_phase) {
        nack = i2c_start_transfer(s->bus, v >> 1, v & 1) != 0;
        s->in_xfer = !nack;
        s->addr_phase = false;
    } else if (s->in_xfer) {
        nack = i2c_send(s->bus, v) != 0;
    } else {
        nack = true;
    }
    s->nack = nack;
    bsc_done(s, nack);
}

static uint64_t bsc_read(void *opaque, hwaddr off, unsigned size)
{
    Wave723Bsc *s = opaque;

    switch (off) {
    case BSC_CS:
        return (s->regs[BSC_CS] & ~(BSC_CS_ACK | BSC_CS_SDA | BSC_CS_SCL)) |
               BSC_CS_SDA | BSC_CS_SCL | (s->nack ? BSC_CS_ACK : 0);
    case BSC_SFTRST:
        return 0;
    case BSC_FCR:
        return s->regs[BSC_FCR] & ~0x87;
    default:
        return off < sizeof(s->regs) ? s->regs[off] : 0;
    }
}

static void bsc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Wave723Bsc *s = opaque;

    switch (off) {
    case BSC_CS:
        s->regs[BSC_CS] = val;
        if ((val & BSC_CS_EN) && (val & BSC_CS_CMDMASK)) {
            bsc_cmd(s, val);
        }
        break;
    case BSC_DAT:
        bsc_send(s, val);
        break;
    case BSC_ISR:
        s->regs[BSC_ISR] &= ~(uint8_t)val;
        break;
    case BSC_SFTRST:
        if (val & 1) {
            if (s->in_xfer) {
                i2c_end_transfer(s->bus);
            }
            s->in_xfer = s->addr_phase = s->nack = false;
            s->regs[BSC_ISR] = 0;
        }
        break;
    default:
        if (off < sizeof(s->regs)) {
            s->regs[off] = val;
        }
        break;
    }
    bsc_update(s);
}

static const MemoryRegionOps bsc_ops = {
    .read = bsc_read,
    .write = bsc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

static void bsc_init(Object *obj)
{
    Wave723Bsc *s = WAVE723_BSC(obj);

    memory_region_init_io(&s->iomem, obj, &bsc_ops, s, TYPE_WAVE723_BSC, 0x80);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static void bsc_reset(DeviceState *dev)
{
    Wave723Bsc *s = WAVE723_BSC(dev);
    if (s->in_xfer) i2c_end_transfer(s->bus);
    s->addr_phase = s->in_xfer = s->nack = false;
    memset(s->regs, 0, sizeof(s->regs));
    bsc_update(s);
}

static void bsc_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), bsc_reset);
}

static const TypeInfo bsc_info = {
    .name = TYPE_WAVE723_BSC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Wave723Bsc),
    .instance_init = bsc_init,
    .class_init = bsc_class_init,
};

static void wave723_register_bsc(void)
{
    type_register_static(&bsc_info);
}

type_init(wave723_register_bsc)
