/* SPDX-License-Identifier: GPL-2.0-only */
/* BCM2156 modem startup registers observed in stock XXKG1. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "wave723_modem.h"

typedef struct ConfigRange {
    const char *name;
    hwaddr base;
    unsigned words;
    uint32_t writable;
    bool cpu_local;
} ConfigRange;

/* These are configuration registers. Keep unrelated offsets off the bus. */
static const ConfigRange config_ranges[] = {
    { "bus-master-priority", 0x08010010, 15, UINT32_MAX },
    { "dsp-shared-memory-config", 0x08130000, 4, UINT32_MAX },
    { "dsp-dma-config", 0x08138000, 2, UINT32_MAX },
    { "rf-clock-config", 0x08169400, 1, 0x7f7fffff },
    { "rf-enable-config", 0x08169408, 1, 0xff7f7f01 },
    { "rf-read-enable-config", 0x0816940c, 1, 0x007f7f11 },
    { "rf-data-config", 0x08169410, 1, 0x000f0f13 },
    { "searcher-clock", 0x08169f88, 1, 0x3ff0ffcc },
    { "turbo-decoder-static-params", 0x0816a140, 1, 0x000f07ff },
    { "irq-trigger-0", 0x08810010, 4, UINT32_MAX, true },
    { "irq-trigger-1", 0x08810110, 4, UINT32_MAX, true },
    { "irq-control", 0x08810024, 1, 0x0000ffef, true },
    { "irq-wake-mask-0", 0x08810140, 1, UINT32_MAX, true },
    { "irq-wake-mask-1", 0x08810148, 1, UINT32_MAX, true },
    /* Reads of ICR are zero. Its write-one-to-clear has no pending sources at reset. */
    { "irq-clear-0", 0x08810008, 1, 0, true },
    { "irq-clear-1", 0x08810108, 1, 0, true },
    { "uart-a-control", 0x08820100, 1, UINT32_MAX },
    { "uart-a-fifo-control", 0x08820108, 1, UINT32_MAX },
    { "uart-b-control", 0x08821100, 1, UINT32_MAX },
    { "oak-phase", 0x08880014, 1, UINT32_MAX },
};

struct Wave723ConfigBank {
    MemoryRegion region;
    uint32_t writable;
    uint32_t values[15];
};

static uint64_t config_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723ConfigBank *bank = opaque;
    return bank->values[offset / 4] >> (8 * (offset % 4));
}

static void config_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723ConfigBank *bank = opaque;
    unsigned shift = 8 * (offset % 4);
    uint32_t mask = (uint32_t)((UINT64_C(1) << (8 * size)) - 1) << shift;
    mask &= bank->writable;
    bank->values[offset / 4] = (bank->values[offset / 4] & ~mask) | ((value << shift) & mask);
}

static const MemoryRegionOps config_ops = {
    .read = config_read, .write = config_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t layer1_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723ModemIO *io = opaque;
    unsigned bank = offset / 0x20;
    switch (offset % 0x20) {
    case 0: return io->layer1_pending[bank] & io->layer1_enable[bank];
    case 4: return io->layer1_pending[bank];
    case 8:
    case 12: return io->layer1_enable[bank];
    default: return 0;
    }
}

static void layer1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723ModemIO *io = opaque;
    unsigned bank = offset / 0x20;
    switch (offset % 0x20) {
    case 8: io->layer1_enable[bank] |= value; break;
    case 12: io->layer1_enable[bank] &= ~value; break;
    case 16: io->layer1_pending[bank] |= value & 3; break;
    case 20: io->layer1_pending[bank] &= ~value; break;
    }
}

static const MemoryRegionOps layer1_ops = {
    .read = layer1_read, .write = layer1_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t sleep_ticks(Wave723ModemIO *io)
{
    if (!(io->control & 1)) {
        return io->ticks;
    }
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - io->epoch;
    return io->ticks + muldiv64(elapsed, 32768, NANOSECONDS_PER_SECOND);
}

static uint64_t sleep_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723ModemIO *io = opaque;
    uint32_t divisor = MAX(1u, io->reload);
    switch (offset) {
    case 0: return io->reload;
    case 4: return divisor - 1 - sleep_ticks(io) % divisor;
    case 8: return io->control;
    case 12: return io->compare;
    case 16: return (uint32_t)(sleep_ticks(io) / divisor);
    default: return 0;
    }
}

static void sleep_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723ModemIO *io = opaque;
    switch (offset) {
    case 0:
        io->reload = value & 0x1fff;
        break;
    case 8:
        io->ticks = sleep_ticks(io);
        io->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        io->control = value & 7;
        break;
    case 12:
        io->compare = value;
        break;
    }
}

static const MemoryRegionOps sleep_ops = {
    .read = sleep_read, .write = sleep_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void wave723_modem_io_init(Wave723ModemIO *io, Object *owner,
                           MemoryRegion *system, MemoryRegion *modem)
{
    io->config = g_new0(Wave723ConfigBank, ARRAY_SIZE(config_ranges));
    for (unsigned i = 0; i < ARRAY_SIZE(config_ranges); ++i) {
        const ConfigRange *range = &config_ranges[i];
        Wave723ConfigBank *bank = &io->config[i];
        bank->writable = range->writable;
        memory_region_init_io(&bank->region, owner, &config_ops, bank,
                              range->name, range->words * 4);
        memory_region_add_subregion_overlap(range->cpu_local ? modem : system,
                                            range->base, &bank->region, 1);
    }
    memory_region_init_io(&io->layer1, owner, &layer1_ops, io, "wave723.layer1-intc", 0x80);
    memory_region_add_subregion(system, 0x0816c280, &io->layer1);
    memory_region_init_io(&io->sleep_timer, owner, &sleep_ops, io, "wave723.sleep-timer", 0x14);
    memory_region_add_subregion(system, 0x08800000, &io->sleep_timer);
}

void wave723_modem_io_reset(Wave723ModemIO *io)
{
    if (!io->config) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(config_ranges); ++i) {
        memset(io->config[i].values, 0, sizeof(io->config[i].values));
    }
    memset(io->layer1_enable, 0, sizeof(io->layer1_enable));
    memset(io->layer1_pending, 0, sizeof(io->layer1_pending));
    io->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    io->ticks = 0;
    io->reload = 0;
    io->control = 0;
    io->compare = 0;
}
