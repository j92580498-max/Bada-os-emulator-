/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WAVE723_DSP_H
#define WAVE723_DSP_H

#include "wave723_intc.h"

typedef struct Wave723Dsp {
    MemoryRegion *ram;
    Wave723Intc *intc;
    uint16_t words[1 << 16];
    uint64_t consumed;
    uint16_t blocked_command;
    bool blocked, checksum_requested;
} Wave723Dsp;

void wave723_dsp_init(Wave723Dsp *dsp, MemoryRegion *ram, Wave723Intc *intc);
void wave723_dsp_reset(Wave723Dsp *dsp);

#endif
