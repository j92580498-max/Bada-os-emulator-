/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WAVE723_ONENAND_H
#define WAVE723_ONENAND_H

#include "system/memory.h"
#include "wave723_nand_geometry.h"

typedef struct Wave723OneNand {
    MemoryRegion region;
    BlockBackend *media;
    BlockBackend *oob;
    BlockBackend *factory;
    int64_t media_size;
    uint16_t regs[0x1000];
    uint8_t boot[512];
    uint8_t data[WAVE723_NAND_PAGE_SIZE];
    uint8_t spare[WAVE723_NAND_SPARE_SIZE];
    uint8_t protection[WAVE723_NAND_BLOCKS];
    bool otp;
} Wave723OneNand;

void wave723_onenand_init(Wave723OneNand *nand, Object *owner,
                         MemoryRegion *memory);
void wave723_onenand_reset(Wave723OneNand *nand);

#endif
