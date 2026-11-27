/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HW_ARM_WAVE723_LCD_H
#define HW_ARM_WAVE723_LCD_H

#include "system/memory.h"

#define WAVE723_LCD_WIDTH 240
#define WAVE723_LCD_HEIGHT 400

typedef struct Wave723Lcd {
    MemoryRegion mmio;
    uint32_t control[0x28 / 4];
    uint8_t rgb[WAVE723_LCD_WIDTH * WAVE723_LCD_HEIGHT * 3];
    uint16_t x0, x1, y0, y1, x, y;
    uint8_t command, pixel_format, parameter_count, parameters[4];
    bool awake, display_on;
    uint64_t writes;
} Wave723Lcd;

void wave723_lcd_init(Wave723Lcd *lcd, Object *owner, MemoryRegion *system_memory);
void wave723_lcd_reset(Wave723Lcd *lcd);
bool wave723_lcd_capture(const Wave723Lcd *lcd, const char *image);

#endif
