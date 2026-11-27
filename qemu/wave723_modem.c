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
} ConfigRange;

/* These are configuration registers. Keep unrelated offsets off the bus. */
static const ConfigRange config_ranges[] = {
    { "bus-master-priority", 0x08010010, 15, UINT32_MAX },
    { "dsp-shared-memory-config", 0x08130000, 4, UINT32_MAX },
    { "dsp-dma-config", 0x08138000, 2, UINT32_MAX },
    { "ap-power-modes", 0x081401f4, 1, UINT32_MAX },
    { "rf-clock-config", 0x08169400, 1, 0x7f7fffff },
    { "rf-enable-config", 0x08169408, 1, 0xff7f7f01 },
    { "rf-read-enable-config", 0x0816940c, 1, 0x007f7f11 },
    { "rf-data-config", 0x08169410, 1, 0x000f0f13 },
    { "searcher-clock", 0x08169f88, 1, 0x3ff0ffcc },
    { "turbo-decoder-static-params", 0x0816a140, 1, 0x000f07ff },
    { "uart-a-control", 0x08820100, 1, UINT32_MAX },
    { "uart-a-fifo-control", 0x08820108, 1, UINT32_MAX },
    { "uart-b-control", 0x08821100, 1, UINT32_MAX },
    { "syscfg-iocr0", 0x08880000, 1, UINT32_MAX },
    { "syscfg-iocr2", 0x0888000c, 1, ~0x00003000u },
    { "oak-phase", 0x08880014, 1, UINT32_MAX },
    { "syscfg-iocr5", 0x08880024, 1, ~0x06003000u },
    { "syscfg-peripheral-reset", 0x08880030, 1, 0x40000001 },
    { "syscfg-anacr6", 0x08880098, 1, ~0xfff9f800u },
    { "syscfg-anacr10", 0x088800a8, 1, ~0x80203fc0u },
    { "crypto-ahb-clock", 0x08880110, 1, 1 },
    { "des-ahb-clock", 0x08880144, 1, 1 },
    { "rng-ahb-clock", 0x08880150, 1, 1 },
    { "syscfg-iocr10", 0x08880204, 1, 0xac000000 },
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

static void schedule_match(Wave723ModemIO *io)
{
    if (!(io->control & 1)) {
        timer_del(io->match);
        return;
    }
    uint32_t divisor = MAX(1u, io->reload);
    uint64_t ticks = sleep_ticks(io);
    uint64_t counts = (uint32_t)(io->compare - ticks / divisor);
    if (!counts) counts = UINT64_C(1) << 32;
    uint64_t until = counts * divisor - ticks % divisor;
    int64_t ns = muldiv64(until, NANOSECONDS_PER_SECOND, 32768) + 1;
    timer_mod(io->match, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void sleep_match(void *opaque)
{
    Wave723ModemIO *io = opaque;
    wave723_intc_pulse(io->intc, 0);
    schedule_match(io);
}

static void sleep_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723ModemIO *io = opaque;
    uint32_t divisor = MAX(1u, io->reload);
    uint64_t ticks = sleep_ticks(io);
    switch (offset) {
    case 0:
        io->reload = value & 0x1fff;
        io->ticks = (ticks / divisor) * MAX(1u, io->reload);
        break;
    case 8:
        io->ticks = ticks;
        io->control = value & 7;
        break;
    case 12:
        io->compare = value;
        io->ticks = ticks;
        break;
    case 16:
        io->ticks = (uint64_t)(uint32_t)value * divisor + ticks % divisor;
        break;
    default: return;
    }
    io->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    schedule_match(io);
}

static const MemoryRegionOps sleep_ops = {
    .read = sleep_read, .write = sleep_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void wave723_modem_io_init(Wave723ModemIO *io, Object *owner,
                           MemoryRegion *system, Wave723Intc *intc)
{
    io->intc = intc;
    io->match = timer_new_ns(QEMU_CLOCK_VIRTUAL, sleep_match, io);
    io->config = g_new0(Wave723ConfigBank, ARRAY_SIZE(config_ranges));
    for (unsigned i = 0; i < ARRAY_SIZE(config_ranges); ++i) {
        const ConfigRange *range = &config_ranges[i];
        Wave723ConfigBank *bank = &io->config[i];
        bank->writable = range->writable;
        memory_region_init_io(&bank->region, owner, &config_ops, bank,
                              range->name, range->words * 4);
        memory_region_add_subregion(system, range->base, &bank->region);
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
    timer_del(io->match);
}
