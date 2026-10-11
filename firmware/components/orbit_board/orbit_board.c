#include "orbit_board.h"

#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "board";
static int s_volume = 70;

static void board_wake_button_gpio_init(void)
{
    /* Single button: BOOT (GPIO0), active-low with internal pull-up */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ORBIT_WAKE_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

esp_err_t orbit_board_init(void)
{
    /* Status LED (the WS2812 lamp on GPIO18 from the original firmware is removed) */
    gpio_config_t led = {
        .pin_bit_mask = (1ULL << ORBIT_STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(ORBIT_STATUS_LED_GPIO, 0);

    board_wake_button_gpio_init();

    ESP_LOGI(TAG, "Board init OK (bread-compact-wifi pinout, 1-button wake)");
    ESP_LOGI(TAG, "  mic I2S: WS=%d SCK=%d DIN=%d | spk I2S: DOUT=%d BCLK=%d LRCK=%d",
             ORBIT_MIC_GPIO_WS, ORBIT_MIC_GPIO_SCK, ORBIT_MIC_GPIO_DIN,
             ORBIT_SPK_GPIO_DOUT, ORBIT_SPK_GPIO_BCLK, ORBIT_SPK_GPIO_LRCK);
    ESP_LOGI(TAG, "  button: wake=BOOT GPIO%d (only button; volume by voice) | OLED: SDA=%d SCL=%d | LED=%d",
             ORBIT_WAKE_BUTTON_GPIO,
             ORBIT_DISPLAY_SDA_PIN, ORBIT_DISPLAY_SCL_PIN, ORBIT_STATUS_LED_GPIO);
    ESP_LOGI(TAG, "  lamp (GPIO18 WS2812): removed by design");
    return ESP_OK;
}

bool orbit_board_wake_button_pressed(void)
{
    return gpio_get_level(ORBIT_WAKE_BUTTON_GPIO) == 0; /* active-low */
}

int orbit_board_get_volume(void)
{
    return s_volume;
}

void orbit_board_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
}
