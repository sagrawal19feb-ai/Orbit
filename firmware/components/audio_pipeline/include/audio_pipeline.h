#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Audio pipeline: I2S mic capture (16 kHz) + speaker playback (24 kHz).
 * Simplex I2S, same wiring as the original firmware (NoAudioCodec):
 *   mic:   WS=GPIO4  SCK=GPIO5  DIN=GPIO6   (I2S_NUM_1, master)
 *   speaker: DOUT=GPIO7  BCLK=GPIO15  LRCK=GPIO16  (I2S_NUM_0, master)
 * Volume is software-scaled on the TX path.
 */

typedef void (*audio_mic_cb_t)(const int16_t *pcm, size_t frames);

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start_mic(audio_mic_cb_t cb);
esp_err_t audio_pipeline_stop_mic(void);

/* write 24 kHz mono PCM to the speaker (volume-scaled) */
esp_err_t audio_pipeline_write_spk(const int16_t *pcm24k, size_t frames);
/* write 16 kHz mono PCM — resampled to 24 kHz internally (linear) */
esp_err_t audio_pipeline_write_spk_16k(const int16_t *pcm16k, size_t frames);

void audio_pipeline_set_volume(int volume); /* 0..100 */
int  audio_pipeline_get_volume(void);

/* smoothed audio levels 0..1 (for the display's voice-sensitive orb) */
float audio_pipeline_get_mic_level(void);
float audio_pipeline_get_output_level(void);
