/* SPDX-License-Identifier: GPL-2.0-only */
/* Broadcom RIP/DSP shared command mailbox used by XXKG1 AMSS. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "wave723_dsp.h"

#define PAGE0 0x2000
#define PAGE5 0xc000
#define CMD_ENTRIES 128
#define STATUS_ENTRIES 32
#define CMD_IN (8 * CMD_ENTRIES)
#define CMD_OUT (CMD_IN + 2)
#define STATUS (CMD_OUT + 2)
#define STATUS_IN (STATUS + 8 * STATUS_ENTRIES)
#define STATUS_OUT (STATUS_IN + 2)

static bool post_status(Wave723Dsp *dsp, uint8_t *page, uint16_t id, uint16_t a,
                        uint16_t b, uint16_t c)
{
    unsigned in = lduw_le_p(page + STATUS_IN), out = lduw_le_p(page + STATUS_OUT);
    unsigned next = (in + 1) % STATUS_ENTRIES;
    if (in >= STATUS_ENTRIES || out >= STATUS_ENTRIES || next == out) {
        return false;
    }
    uint8_t *entry = page + STATUS + 8 * in;
    stw_le_p(entry, id);
    stw_le_p(entry + 2, a);
    stw_le_p(entry + 4, b);
    stw_le_p(entry + 6, c);
    stw_le_p(page + STATUS_IN, next);
    memory_region_set_dirty(dsp->ram, PAGE5 + STATUS + 8 * in, 8);
    memory_region_set_dirty(dsp->ram, PAGE5 + STATUS_IN, 2);
    wave723_intc_pulse(dsp->intc, 1); /* RIPCMD */
    return true;
}

static void read_taps(uint16_t *taps, unsigned count, const uint8_t *source)
{
    for (unsigned i = 0; i < count; ++i) {
        taps[i] = lduw_le_p(source + 2 * i);
    }
}

static void dsp_doorbell(void *opaque, int line, int level)
{
    Wave723Dsp *dsp = opaque;
    uint8_t *ram = memory_region_get_ram_ptr(dsp->ram);
    uint8_t *page = ram + PAGE5;
    unsigned in = lduw_le_p(page + CMD_IN), out = lduw_le_p(page + CMD_OUT);
    if (!level || in >= CMD_ENTRIES || out >= CMD_ENTRIES) {
        return;
    }
    while (out != in) {
        const uint8_t *cmd = page + 8 * out;
        unsigned id = lduw_le_p(cmd);
        unsigned arg0 = lduw_le_p(cmd + 2), arg1 = lduw_le_p(cmd + 4);
        switch (id) {
        case 0x0b: /* COMMAND_READ */
            if (!post_status(dsp, page, 3, arg0, arg1, dsp->words[arg0])) {
                return;
            }
            break;
        case 0x0c: /* COMMAND_WRITE */
            dsp->words[arg0] = arg1;
            break;
        case 0x0e: /* COMMAND_SYNC */
            if (!post_status(dsp, page, 4, arg0, 0, 0)) {
                return;
            }
            break;
        case 0x1d: /* COMMAND_DEBUG_MODE: latch the request for a ROM checksum. */
            dsp->checksum_requested = true;
            break;
        case 0x1f: /* COMMAND_GPIO_FRAME_ACTIVE */
            dsp->audio.gpio_address = arg0;
            dsp->audio.gpio_value = arg1;
            dsp->audio.gpio_mask = lduw_le_p(cmd + 6);
            break;
        case 0x40: case 0x41: case 0x42: case 0x43: /* Sidetone filter */
        case 0x45: case 0x46: case 0x47: case 0x48: { /* Echo canceller */
            bool sidetone = id <= 0x43;
            unsigned first = 3 * (id - (sidetone ? 0x40 : 0x45));
            uint16_t *taps = sidetone ? dsp->audio.sidetone : dsp->audio.echo;
            read_taps(taps + first, MIN(3u, 10u - first), cmd + 2);
            break;
        }
        case 0x5c: /* COMMAND_VOICE_FILTER_COEFS */
            if (arg0 <= ARRAY_SIZE(dsp->audio.voice_tx) && arg1 <= ARRAY_SIZE(dsp->audio.voice_rx)) {
                /* XXKG1 populates these page-0 slots at 0x80630184 and 0x8063010a. */
                read_taps(dsp->audio.voice_tx, arg0, ram + PAGE0 + 0x6e8);
                read_taps(dsp->audio.voice_rx, arg1, ram + PAGE0 + 0x71a);
                dsp->audio.tx_count = arg0;
                dsp->audio.rx_count = arg1;
                break;
            }
            /* fall through */
        default:
            dsp->blocked = true;
            dsp->blocked_command = id;
            return;
        }
        out = (out + 1) % CMD_ENTRIES;
        stw_le_p(page + CMD_OUT, out);
        memory_region_set_dirty(dsp->ram, PAGE5 + CMD_OUT, 2);
        ++dsp->consumed;
    }
    dsp->blocked = false;
}

void wave723_dsp_init(Wave723Dsp *dsp, MemoryRegion *ram, Wave723Intc *intc)
{
    dsp->ram = ram;
    dsp->intc = intc;
    intc->rip = qemu_allocate_irq(dsp_doorbell, dsp, 0);
}

void wave723_dsp_reset(Wave723Dsp *dsp)
{
    memset(dsp->words, 0, sizeof(dsp->words));
    memset(&dsp->audio, 0, sizeof(dsp->audio));
    dsp->consumed = 0;
    dsp->blocked_command = 0;
    dsp->blocked = dsp->checksum_requested = false;
}
