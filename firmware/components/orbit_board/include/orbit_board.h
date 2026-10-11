#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>

/*
 * Orbit board pin config — IDENTICAL to the original device firmware
 * (xiaozhi bread-compact-wifi), except:
 *  - the WS2812 lamp on GPIO18 is intentionally NOT present (removed by design)
 *  - only ONE button: BOOT (GPIO0) as the wake button. Volume and everything
 *    else are voice-controlled. The original firmware's touch/volume buttons
 *    (GPIO47/40/39) are intentionally not used.
 */

/* ---- Audio: I2S simplex (separate mic & speaker buses) ---- */
#define ORBIT_AUDIO_INPUT_SAMPLE_RATE  16000
#define ORBIT_AUDIO_OUTPUT_SAMPLE_RATE 24000

#define ORBIT_MIC_GPIO_WS   GPIO_NUM_4
#define ORBIT_MIC_GPIO_SCK  GPIO_NUM_5
#define ORBIT_MIC_GPIO_DIN  GPIO_NUM_6

#define ORBIT_SPK_GPIO_DOUT GPIO_NUM_7
#define ORBIT_SPK_GPIO_BCLK GPIO_NUM_15
#define ORBIT_SPK_GPIO_LRCK GPIO_NUM_16

/* ---- Button: BOOT (GPIO0) — the only button, used as the wake button ----
 * (active-low, internal pull-up). Press to wake Orbit without saying the word. */
#define ORBIT_WAKE_BUTTON_GPIO GPIO_NUM_0

/* ---- OLED display: 0.91" SSD1306 128x32 @ 0x3C, I2C0 400 kHz ----
 * (drop-in swap for the original 128x64 panel — same pins) */
#define ORBIT_DISPLAY_SDA_PIN GPIO_NUM_41
#define ORBIT_DISPLAY_SCL_PIN GPIO_NUM_42
#define ORBIT_DISPLAY_WIDTH   128
#define ORBIT_DISPLAY_HEIGHT  32

/* ---- Status LED ---- */
#define ORBIT_STATUS_LED_GPIO GPIO_NUM_48

/* ---- Lamp: intentionally absent (original firmware had WS2812 on GPIO18) ---- */

esp_err_t orbit_board_init(void);

/* true while the wake button is held down (active-low) */
bool orbit_board_wake_button_pressed(void);

int  orbit_board_get_volume(void);
void orbit_board_set_volume(int volume); /* 0..100 — voice-controlled via "Orbit, volume N" */
