#include "ssd1306.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "ssd1306";
static i2c_port_t s_port = I2C_NUM_0;
static uint8_t s_addr = SSD1306_I2C_ADDR;
static bool s_ok = false;

static esp_err_t i2c_write(const uint8_t *data, size_t len)
{
    return i2c_master_write_to_device(s_port, s_addr, data, len,
                                      pdMS_TO_TICKS(200));
}

static esp_err_t ssd1306_cmd(uint8_t c)
{
    uint8_t buf[2] = { 0x00, c }; /* control byte 0x00 = command */
    return i2c_write(buf, sizeof(buf));
}

static esp_err_t ssd1306_cmd2(uint8_t c, uint8_t arg)
{
    uint8_t buf[3] = { 0x00, c, arg };
    return i2c_write(buf, sizeof(buf));
}

esp_err_t ssd1306_init(int sda_gpio, int scl_gpio, int freq_hz)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = (uint32_t)freq_hz },
    };
    ESP_ERROR_CHECK(i2c_param_config(s_port, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(s_port, I2C_MODE_MASTER, 0, 0, 0));

    /* probe both common OLED addresses: 0x3C first, then 0x3D */
    esp_err_t err = ssd1306_cmd(0xAE); /* display off */
    if (err != ESP_OK) {
        s_addr = 0x3D;
        err = ssd1306_cmd(0xAE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no OLED ACK at 0x3C or 0x3D (SDA=%d SCL=%d) — display disabled",
                 sda_gpio, scl_gpio);
        return err;
    }

    /* init sequence for 128x32 — PAGE addressing mode works on both the
     * SSD1306 and the SH1106 (the 0.91" panels are either; horizontal
     * addressing is NOT supported by the SH1106) */
    err  = ssd1306_cmd(0xAE);        /* display off */
    err |= ssd1306_cmd2(0xD5, 0x80); /* clock divide */
    err |= ssd1306_cmd2(0xA8, 0x1F); /* multiplex ratio = 32 */
    err |= ssd1306_cmd2(0xD3, 0x00); /* display offset */
    err |= ssd1306_cmd(0x40);        /* start line 0 */
    err |= ssd1306_cmd2(0x8D, 0x14); /* charge pump on (ignored by SH1106) */
    err |= ssd1306_cmd2(0x20, 0x02); /* PAGE addressing mode */
    err |= ssd1306_cmd(0xA1);        /* segment remap */
    err |= ssd1306_cmd(0xC8);        /* COM scan direction */
    err |= ssd1306_cmd2(0xDA, 0x02); /* COM pins (128x32) */
    err |= ssd1306_cmd2(0x81, 0x8F); /* contrast */
    err |= ssd1306_cmd2(0xD9, 0xF1); /* pre-charge */
    err |= ssd1306_cmd2(0xDB, 0x40); /* VCOMH level */
    err |= ssd1306_cmd(0xA4);        /* display follows RAM */
    err |= ssd1306_cmd(0xA6);        /* normal (not inverted) */
    err |= ssd1306_cmd(0xAF);        /* display on */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed");
        return err;
    }
    s_ok = true;
    ESP_LOGI(TAG, "OLED 128x32 ready at I2C 0x%02X (page addressing)", s_addr);

    /* clear the screen */
    static const uint8_t zeros[SSD1306_WIDTH] = {0};
    for (uint8_t p = 0; p < SSD1306_PAGES; p++) {
        (void)ssd1306_write_window(0, SSD1306_WIDTH - 1, p, p, zeros, sizeof(zeros));
    }
    return ESP_OK;
}

esp_err_t ssd1306_display_on(bool on)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    return ssd1306_cmd(on ? 0xAF : 0xAE);
}

esp_err_t ssd1306_set_contrast(uint8_t contrast)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    return ssd1306_cmd2(0x81, contrast);
}

esp_err_t ssd1306_write_window(uint8_t col0, uint8_t col1,
                               uint8_t page0, uint8_t page1,
                               const uint8_t *data, size_t len)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    int cols = (int)col1 - (int)col0 + 1;
    int pages = (int)page1 - (int)page0 + 1;
    if (cols <= 0 || pages <= 0 || (size_t)cols * (size_t)pages > len) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int p = 0; p < pages; p++) {
        /* page addressing: select page + column, then stream the page's bytes */
        esp_err_t err = ssd1306_cmd(0xB0 | (uint8_t)((page0 + p) & 0x07));
        err |= ssd1306_cmd(0x00 | (uint8_t)(col0 & 0x0F)); /* column low nibble */
        err |= ssd1306_cmd(0x10 | (uint8_t)(col0 >> 4));   /* column high nibble */
        if (err != ESP_OK) {
            return err;
        }
        uint8_t *buf = heap_caps_malloc((size_t)cols + 1, MALLOC_CAP_8BIT);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        buf[0] = 0x40; /* control byte 0x40 (Co=0, D/C#=1) */
        memcpy(buf + 1, data + (size_t)p * (size_t)cols, (size_t)cols);
        err = i2c_write(buf, (size_t)cols + 1);
        free(buf);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
