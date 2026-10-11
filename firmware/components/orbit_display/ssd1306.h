#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal SSD1306 I2C driver (128x32) — custom, no external component. */

#define SSD1306_I2C_ADDR   0x3C
#define SSD1306_WIDTH      128
#define SSD1306_HEIGHT     32
#define SSD1306_PAGES      (SSD1306_HEIGHT / 8)

esp_err_t ssd1306_init(int sda_gpio, int scl_gpio, int freq_hz);
esp_err_t ssd1306_display_on(bool on);
esp_err_t ssd1306_set_contrast(uint8_t contrast);

/* write a window: columns [col0..col1], pages [page0..page1], data in
 * page-major order (page0 cols, page1 cols, ...) — matches the framebuffer */
esp_err_t ssd1306_write_window(uint8_t col0, uint8_t col1,
                               uint8_t page0, uint8_t page1,
                               const uint8_t *data, size_t len);
