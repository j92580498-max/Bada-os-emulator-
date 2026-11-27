/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "wave723_lcd.h"

#define WAVE723_LCDC_BASE 0x08030000
#define LCDC_CMDR 0x00
#define LCDC_DATR 0x04
#define LCDC_STATUS 0x1c

static uint64_t lcdc_read(void *opaque, hwaddr offset, unsigned size)
{
    Wave723Lcd *lcd = opaque;

    if (offset == LCDC_STATUS) {
        return (1U << 31) | (1U << 14); /* FIFO empty, read ready */
    }
    return lcd->control[offset / 4];
}

static void lcd_command(Wave723Lcd *lcd, uint8_t command)
{
    lcd->command = command;
    lcd->parameter_count = 0;
    switch (command) {
    case 0x10:
        lcd->awake = false;
        break;
    case 0x11:
        lcd->awake = true;
        break;
    case 0x28:
        lcd->display_on = false;
        break;
    case 0x29:
        lcd->display_on = true;
        break;
    case 0x2c:
        lcd->x = lcd->x0;
        lcd->y = lcd->y0;
        break;
    }
}

static void lcd_pixel(Wave723Lcd *lcd, uint16_t rgb565)
{
    if (lcd->x0 > lcd->x1 || lcd->y0 > lcd->y1) {
        return;
    }
    if (lcd->x < WAVE723_LCD_WIDTH && lcd->y < WAVE723_LCD_HEIGHT) {
        uint8_t *pixel = lcd->rgb + (lcd->y * WAVE723_LCD_WIDTH + lcd->x) * 3;

        pixel[0] = (rgb565 >> 11) * 255 / 31;
        pixel[1] = ((rgb565 >> 5) & 63) * 255 / 63;
        pixel[2] = (rgb565 & 31) * 255 / 31;
        ++lcd->writes;
    }
    if (lcd->x == lcd->x1) {
        lcd->x = lcd->x0;
        lcd->y = lcd->y == lcd->y1 ? lcd->y0 : lcd->y + 1;
    } else {
        ++lcd->x;
    }
}

static void lcd_data(Wave723Lcd *lcd, uint32_t data)
{
    switch (lcd->command) {
    case 0x2a:
    case 0x2b:
        if (lcd->parameter_count == 4) {
            break;
        }
        lcd->parameters[lcd->parameter_count++] = data;
        if (lcd->parameter_count == 4) {
            uint16_t first = (lcd->parameters[0] << 8) | lcd->parameters[1];
            uint16_t last = (lcd->parameters[2] << 8) | lcd->parameters[3];

            if (lcd->command == 0x2a) {
                lcd->x0 = first;
                lcd->x1 = last;
            } else {
                lcd->y0 = first;
                lcd->y1 = last;
            }
        }
        break;
    case 0x2c:
    case 0x3c:
        if ((lcd->pixel_format & 7) == 5) {
            lcd_pixel(lcd, data);
        }
        break;
    case 0x3a:
        lcd->pixel_format = data;
        break;
    }
}

static void lcdc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Wave723Lcd *lcd = opaque;

    switch (offset) {
    case LCDC_CMDR:
        lcd_command(lcd, value);
        break;
    case LCDC_DATR:
        lcd_data(lcd, value);
        break;
    case LCDC_STATUS:
        break;
    default:
        lcd->control[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps lcdc_ops = {
    .read = lcdc_read,
    .write = lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void wave723_lcd_init(Wave723Lcd *lcd, Object *owner, MemoryRegion *system_memory)
{
    memory_region_init_io(&lcd->mmio, owner, &lcdc_ops, lcd, "wave723.lcdc", 0x28);
    memory_region_add_subregion(system_memory, WAVE723_LCDC_BASE, &lcd->mmio);
}

void wave723_lcd_reset(Wave723Lcd *lcd)
{
    memset(lcd->rgb, 0, sizeof(lcd->rgb));
    memset(lcd->control, 0, sizeof(lcd->control));
    lcd->command = lcd->parameter_count = 0;
    lcd->x = lcd->y = lcd->x0 = lcd->y0 = 0;
    lcd->x1 = WAVE723_LCD_WIDTH - 1;
    lcd->y1 = WAVE723_LCD_HEIGHT - 1;
    lcd->pixel_format = 0x66;
    lcd->awake = lcd->display_on = false;
    lcd->writes = 0;
}

bool wave723_lcd_capture(const Wave723Lcd *lcd, const char *image)
{
    g_autofree char *path = g_strconcat(image, ".panel.rgb", NULL);
    g_autoptr(GError) error = NULL;

    if (!lcd->writes || !lcd->awake || !lcd->display_on) {
        return false;
    }
    if (!g_file_set_contents(path, (const char *)lcd->rgb, sizeof(lcd->rgb), &error)) {
        error_report("Wave 723 LCD capture: %s", error->message);
        return false;
    }
    return true;
}
