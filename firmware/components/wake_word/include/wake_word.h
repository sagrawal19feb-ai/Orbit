#pragma once

#include "esp_err.h"

/*
 * Wake-word detection: ESP-SR MultiNet6 model "mn6_en" (wake word: "Orbit"),
 * loaded from the assets partition (srmodels.bin). Implemented in Phase 3.
 */

typedef void (*wake_word_cb_t)(void);

esp_err_t wake_word_init(void);
esp_err_t wake_word_start(wake_word_cb_t on_wake);
void wake_word_stop(void);
