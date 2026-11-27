/* SPDX-License-Identifier: GPL-2.0-only */
/* Samsung 4 KiB SLC OneNAND as accessed by the Wave 723 FSR driver. */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "wave723_onenand.h"

#define ONENAND_BASE 0x00400000
#define ONENAND_WINDOW 0x20000
#define ONENAND_DATARAM 0x400
#define ONENAND_SPARERAM 0x10020
#define ONENAND_REG_BASE 0x1e000

/* Word offsets from the start of the register window. */
enum {
    MANUFACTURER_ID = 0x000, DEVICE_ID, VERSION_ID,
    DATA_BUFFER_SIZE, BOOT_BUFFER_SIZE, NUM_BUFFERS, TECHNOLOGY,
    START_ADDRESS1 = 0x100, START_ADDRESS8 = 0x107,
    START_BUFFER = 0x200,
    COMMAND = 0x220, SYS_CFG1, SYS_CFG2,
    CTRL_STATUS = 0x240, INTERRUPT,
    START_BLOCK = 0x24c, END_BLOCK, WRITE_PROTECT,
};

#define INT_MASTER 0x8000
#define INT_READ 0x0080
#define INT_PROGRAM 0x0040
#define INT_ERASE 0x0020
#define INT_RESET 0x0010
#define STATUS_ERROR 0x0400
#define WP_TIGHT 1
#define WP_LOCKED 2
#define WP_UNLOCKED 4

void wave723_onenand_reset(Wave723OneNand *nand)
{
    memset(nand->regs, 0, sizeof(nand->regs));
    memset(nand->boot, 0xff, sizeof(nand->boot));
    memset(nand->data, 0xff, sizeof(nand->data));
    memset(nand->spare, 0xff, sizeof(nand->spare));
    nand->otp = false;
    nand->regs[MANUFACTURER_ID] = 0x00ec;
    nand->regs[DEVICE_ID] = 0x0050;  /* Samsung 4 Gbit */
    nand->regs[VERSION_ID] = 0x0010;
    nand->regs[DATA_BUFFER_SIZE] = WAVE723_NAND_PAGE_SIZE;
    nand->regs[BOOT_BUFFER_SIZE] = sizeof(nand->boot);
    nand->regs[NUM_BUFFERS] = 1;
    nand->regs[TECHNOLOGY] = 0;  /* SLC */
    nand->regs[INTERRUPT] = INT_MASTER | INT_RESET;
    memset(nand->protection, WP_UNLOCKED, sizeof(nand->protection));
}

static unsigned onenand_block(Wave723OneNand *nand)
{
    return nand->regs[START_ADDRESS1] & (WAVE723_NAND_BLOCKS - 1);
}

static unsigned onenand_page(Wave723OneNand *nand)
{
    unsigned page = (nand->regs[START_ADDRESS8] >> 2) & (WAVE723_NAND_PAGES_PER_BLOCK - 1);
    return onenand_block(nand) * WAVE723_NAND_PAGES_PER_BLOCK + page;
}

static bool onenand_load(Wave723OneNand *nand, bool read_main)
{
    unsigned page = onenand_page(nand);
    int64_t offset = (int64_t)page * WAVE723_NAND_PAGE_SIZE;
    memset(nand->spare, 0xff, sizeof(nand->spare));
    if (read_main) {
        memset(nand->data, 0xff, sizeof(nand->data));
    }
    if (nand->otp) {
        return true;
    }
    if (nand->factory) {
        uint8_t record[WAVE723_NAND_RECORD_SIZE];
        if (blk_pread(nand->factory, (int64_t)page * sizeof(record),
                      sizeof(record), record, 0) < 0) {
            return false;
        }
        /* Holes in a private factory image represent erased NAND. */
        if (read_main) {
            for (unsigned i = 0; i < sizeof(nand->data); ++i) {
                nand->data[i] = ~record[i];
            }
        }
        for (unsigned i = 0; i < sizeof(nand->spare); ++i) {
            nand->spare[i] = ~record[sizeof(nand->data) + i];
        }
    } else if (offset < nand->media_size) {
        if (read_main && blk_pread(nand->media, offset, sizeof(nand->data),
                                   nand->data, 0) < 0) {
            return false;
        }
        if (nand->oob && blk_pread(nand->oob, (int64_t)page * sizeof(nand->spare),
                                   sizeof(nand->spare), nand->spare, 0) < 0) {
            return false;
        }
    }
    return true;
}

static bool onenand_writable(Wave723OneNand *nand)
{
    return nand->factory && blk_is_writable(nand->factory) && !nand->otp &&
           nand->protection[onenand_block(nand)] == WP_UNLOCKED;
}

static bool onenand_program(Wave723OneNand *nand, bool main)
{
    uint8_t record[WAVE723_NAND_RECORD_SIZE];
    int64_t offset = (int64_t)onenand_page(nand) * sizeof(record);

    if (!onenand_writable(nand) ||
        blk_pread(nand->factory, offset, sizeof(record), record, 0) < 0) {
        return false;
    }
    /* Programming can only clear bits in the decoded main and spare areas. */
    if (main) {
        for (unsigned i = 0; i < sizeof(nand->data); ++i) {
            record[i] |= (uint8_t)~nand->data[i];
        }
    }
    for (unsigned i = 0; i < sizeof(nand->spare); ++i) {
        record[sizeof(nand->data) + i] |= (uint8_t)~nand->spare[i];
    }
    return blk_pwrite(nand->factory, offset, sizeof(record), record, 0) == 0;
}

static bool onenand_erase(Wave723OneNand *nand)
{
    const int64_t bytes = WAVE723_NAND_PAGES_PER_BLOCK * WAVE723_NAND_RECORD_SIZE;
    return onenand_writable(nand) &&
           blk_pwrite_zeroes(nand->factory, onenand_block(nand) * bytes,
                             bytes, BDRV_REQ_MAY_UNMAP) == 0;
}

static void onenand_command(Wave723OneNand *nand, uint16_t command)
{
    uint16_t interrupt = INT_MASTER;
    unsigned lock_block = nand->regs[START_BLOCK] & (WAVE723_NAND_BLOCKS - 1);
    bool ok = true;

    nand->regs[CTRL_STATUS] = 0;
    switch (command) {
    case 0x00:  /* Page read */
    case 0x03:  /* Superload */
    case 0x13:  /* Spare read */
        ok = onenand_load(nand, command != 0x13);
        interrupt |= INT_READ;
        break;
    case 0x65:  /* OTP access */
        nand->otp = true;
        interrupt |= INT_RESET;
        break;
    case 0xf0:  /* Reset */
    case 0xf3:  /* Hot reset */
        wave723_onenand_reset(nand);
        return;
    case 0x23:  /* Unlock */
    case 0x2a:  /* Lock */
    case 0x2c:  /* Lock tight */
        if (nand->protection[lock_block] != WP_TIGHT) {
            nand->protection[lock_block] = command == 0x23 ? WP_UNLOCKED :
                                           command == 0x2a ? WP_LOCKED : WP_TIGHT;
        }
        break;
    case 0x27:  /* Unlock all */
        for (unsigned i = 0; i < WAVE723_NAND_BLOCKS; ++i) {
            if (nand->protection[i] != WP_TIGHT) {
                nand->protection[i] = WP_UNLOCKED;
            }
        }
        break;
    case 0x80:  /* Program */
    case 0x1a:  /* Program spare */
    case 0x7f:  /* Cache program */
        ok = onenand_program(nand, command != 0x1a);
        interrupt |= INT_PROGRAM;
        break;
    case 0x94:  /* Erase */
        ok = onenand_erase(nand);
        interrupt |= INT_ERASE;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "wave723 OneNAND command %#x\n", command);
        ok = false;
        break;
    }
    nand->regs[CTRL_STATUS] = ok ? 0 : STATUS_ERROR;
    nand->regs[INTERRUPT] = interrupt;
}

static uint8_t *onenand_buffer(Wave723OneNand *nand, hwaddr offset)
{
    if (offset < sizeof(nand->boot)) {
        return &nand->boot[offset];
    }
    if (offset >= ONENAND_DATARAM && offset < ONENAND_DATARAM + sizeof(nand->data)) {
        return (uint8_t *)nand->data + offset - ONENAND_DATARAM;
    }
    if (offset >= ONENAND_SPARERAM && offset < ONENAND_SPARERAM + sizeof(nand->spare)) {
        return (uint8_t *)nand->spare + offset - ONENAND_SPARERAM;
    }
    return NULL;
}

static uint64_t onenand_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723OneNand *nand = opaque;
    uint64_t result = 0;

    for (unsigned i = 0; i < size; ++i, ++offset) {
        uint8_t byte = 0xff;
        if (offset >= ONENAND_REG_BASE) {
            unsigned reg = (offset - ONENAND_REG_BASE) / 2;
            uint16_t val = reg == WRITE_PROTECT ? nand->protection[onenand_block(nand)] :
                                                  nand->regs[reg];
            byte = val >> (8 * (offset & 1));
        } else {
            uint8_t *buffer = onenand_buffer(nand, offset);
            if (buffer) {
                byte = *buffer;
            }
        }
        result |= (uint64_t)byte << (8 * i);
    }
    return result;
}

static bool onenand_reg_writable(unsigned reg)
{
    return (reg >= START_ADDRESS1 && reg <= START_ADDRESS8) ||
           reg == START_BUFFER || (reg >= COMMAND && reg <= SYS_CFG2) ||
           reg == INTERRUPT || reg == START_BLOCK || reg == END_BLOCK;
}

static void onenand_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    Wave723OneNand *nand = opaque;
    const hwaddr command = ONENAND_REG_BASE + 2 * COMMAND;

    for (unsigned i = 0; i < size; ++i) {
        hwaddr address = offset + i;
        uint8_t byte = value >> (8 * i);

        if (address >= ONENAND_REG_BASE) {
            unsigned reg = (address - ONENAND_REG_BASE) / 2;
            unsigned shift = 8 * (address & 1);
            if (onenand_reg_writable(reg)) {
                nand->regs[reg] = (nand->regs[reg] & ~(0xffu << shift)) |
                                  (byte << shift);
            }
        } else {
            uint8_t *buffer = onenand_buffer(nand, address);
            if (buffer) {
                *buffer = byte;
            }
        }
    }
    if (offset <= command && offset + size > command) {
        onenand_command(nand, nand->regs[COMMAND]);
    } else if (offset == 0 && size == 2 && value == 0x90) {
        nand->boot[0] = nand->regs[MANUFACTURER_ID];
        nand->boot[1] = 0;
        nand->boot[2] = nand->regs[DEVICE_ID];
        nand->boot[3] = 0;
    }
}

static const MemoryRegionOps onenand_ops = {
    .read = onenand_read,
    .write = onenand_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void wave723_onenand_init(Wave723OneNand *nand, Object *owner,
                         MemoryRegion *memory)
{
    nand->media = blk_by_name("wave723-flash");
    nand->oob = blk_by_name("wave723-spare");
    nand->factory = blk_by_name("wave723-factory");
    if (nand->factory && (nand->media || nand->oob ||
        blk_getlength(nand->factory) != (int64_t)WAVE723_NAND_PAGES * WAVE723_NAND_RECORD_SIZE)) {
        error_report("Invalid or conflicting Wave 723 factory flash image.");
        exit(1);
    }
    if (nand->media) {
        nand->media_size = blk_getlength(nand->media);
        if (nand->media_size <= 0 || nand->media_size > 512 * MiB ||
            nand->media_size % WAVE723_NAND_PAGE_SIZE) {
            error_report("Wave 723 OneNAND requires a page-aligned image of up to 512 MiB.");
            exit(1);
        }
    }
    if (nand->oob && (!nand->media || blk_getlength(nand->oob) !=
            nand->media_size / WAVE723_NAND_PAGE_SIZE * WAVE723_NAND_SPARE_SIZE)) {
        error_report("Wave 723 spare image requires 128 bytes per main-area page.");
        exit(1);
    }
    memory_region_init_io(&nand->region, owner, &onenand_ops,
                          nand, "wave723.onenand", ONENAND_WINDOW);
    memory_region_add_subregion(memory, ONENAND_BASE, &nand->region);
}
