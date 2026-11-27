/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WAVE723_GPT_H
#define WAVE723_GPT_H

#include "hw/core/ptimer.h"
#include "wave723_intc.h"

typedef struct Wave723Gpt Wave723Gpt;
typedef struct Wave723GptChannel {
    Wave723Gpt *gpt;
    ptimer_state *timer;
    uint32_t csr, reload;
} Wave723GptChannel;

struct Wave723Gpt {
    MemoryRegion mmio;
    Wave723GptChannel channel[6];
    Wave723Intc *intc;
};

void wave723_gpt_init(Wave723Gpt *gpt, Object *owner, MemoryRegion *memory, Wave723Intc *intc);
void wave723_gpt_reset(Wave723Gpt *gpt);

#endif
