/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WAVE723_MODEM_H
#define WAVE723_MODEM_H

#include "wave723_intc.h"

typedef struct Wave723ConfigBank Wave723ConfigBank;

typedef struct Wave723ModemIO {
    Wave723ConfigBank *config;
    MemoryRegion cipher, cipher_ram;
    uint32_t cipher_config;
    MemoryRegion layer1;
    uint32_t layer1_enable[4];
    uint32_t layer1_pending[4];
    MemoryRegion sleep_timer;
    Wave723Intc *intc;
    QEMUTimer *match;
    int64_t epoch;
    uint64_t ticks;
    uint32_t reload;
    uint32_t control;
    uint32_t compare;
} Wave723ModemIO;

void wave723_modem_io_init(Wave723ModemIO *io, Object *owner,
                           MemoryRegion *system, Wave723Intc *intc);
void wave723_modem_io_reset(Wave723ModemIO *io);

#endif
