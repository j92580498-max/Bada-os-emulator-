/* SPDX-License-Identifier: GPL-2.0-only */
/* BCM215x general-purpose down-counters. */
#include "qemu/osdep.h"
#include "wave723_gpt.h"

#define GPT_ENABLE (1U << 31)
#define GPT_FAST (1U << 30)
#define GPT_IRQ_ENABLE (1U << 27)
#define GPT_IRQ2 (1U << 26)
#define GPT_MHZ (1U << 25)
#define GPT_POWER (1U << 24)
#define GPT_FLAG 1U

static void update_irq(Wave723Gpt *gpt)
{
    if (!gpt->intc) return;
    bool line[2] = { false, false };
    for (unsigned i = 0; i < ARRAY_SIZE(gpt->channel); ++i) {
        uint32_t csr = gpt->channel[i].csr;
        if ((csr & (GPT_IRQ_ENABLE | GPT_FLAG)) == (GPT_IRQ_ENABLE | GPT_FLAG)) {
            line[!!(csr & GPT_IRQ2)] = true;
        }
    }
    wave723_intc_set(gpt->intc, 11, line[0]);
    wave723_intc_set(gpt->intc, 68, line[1]);
}

static void expired(void *opaque)
{
    Wave723GptChannel *ch = opaque;
    ch->csr |= GPT_FLAG;
    update_irq(ch->gpt);
}

static uint64_t gpt_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723Gpt *gpt = opaque;
    if (!offset) {
        uint32_t flags = 0;
        for (unsigned i = 0; i < ARRAY_SIZE(gpt->channel); ++i) {
            flags |= (gpt->channel[i].csr & GPT_FLAG) << i;
        }
        return flags;
    }
    Wave723GptChannel *ch = &gpt->channel[(offset - 4) / 16];
    switch (offset % 16) {
    case 4: return ch->csr;
    case 8: return ch->reload;
    case 12: return ptimer_get_count(ch->timer);
    default: return 0;
    }
}

static void gpt_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723Gpt *gpt = opaque;
    if (offset % 16 == 0) return;
    Wave723GptChannel *ch = &gpt->channel[(offset - 4) / 16];
    uint32_t old = ch->csr;
    ptimer_transaction_begin(ch->timer);
    switch (offset % 16) {
    case 4:
        ch->csr = ((uint32_t)value & 0xffc00000) | (old & ~value & GPT_FLAG);
        if ((old ^ value) & (GPT_FAST | GPT_MHZ)) {
            ptimer_set_freq(ch->timer, value & GPT_FAST ? 26000000 :
                                       value & GPT_MHZ ? 1000000 : 32768);
        }
        if ((old ^ value) & GPT_ENABLE) {
            if (value & GPT_ENABLE) {
                ptimer_set_count(ch->timer, MAX(ch->reload, 1u));
                ptimer_run(ch->timer, 0);
            } else {
                ptimer_stop(ch->timer);
            }
        }
        break;
    case 8:
        ch->reload = value;
        ptimer_set_limit(ch->timer, MAX(ch->reload, 1u), 1);
        break;
    case 12:
        ptimer_set_count(ch->timer, value);
        break;
    }
    ptimer_transaction_commit(ch->timer);
    update_irq(gpt);
}

static const MemoryRegionOps gpt_ops = {
    .read = gpt_read, .write = gpt_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void wave723_gpt_init(Wave723Gpt *gpt, Object *owner, MemoryRegion *memory, Wave723Intc *intc)
{
    gpt->intc = intc;
    for (unsigned i = 0; i < ARRAY_SIZE(gpt->channel); ++i) {
        Wave723GptChannel *ch = &gpt->channel[i];
        ch->gpt = gpt;
        ch->timer = ptimer_init(expired, ch, PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
            PTIMER_POLICY_CONTINUOUS_TRIGGER | PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
            PTIMER_POLICY_NO_IMMEDIATE_RELOAD | PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
    }
    memory_region_init_io(&gpt->mmio, owner, &gpt_ops, gpt, "wave723.gpt", 0x60);
    memory_region_add_subregion(memory, 0x08830100, &gpt->mmio);
}

void wave723_gpt_reset(Wave723Gpt *gpt)
{
    for (unsigned i = 0; i < ARRAY_SIZE(gpt->channel); ++i) {
        Wave723GptChannel *ch = &gpt->channel[i];
        /* APPS enters from the firmware loader with its timebase running. */
        ch->csr = i == 0 ? GPT_ENABLE | GPT_POWER : 0;
        ch->reload = i == 0 ? UINT32_MAX : 0;
        ptimer_transaction_begin(ch->timer);
        ptimer_stop(ch->timer);
        ptimer_set_freq(ch->timer, 32768);
        ptimer_set_limit(ch->timer, MAX(ch->reload, 1u), 1);
        if (i == 0) ptimer_run(ch->timer, 0);
        ptimer_transaction_commit(ch->timer);
    }
    update_irq(gpt);
}
