#include "audio_pipeline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "soc/soc_caps.h"

#include "orbit_board.h"

static const char *TAG = "audio";

#define MIC_FRAMES_PER_READ 512
#define DMA_DESC_NUM        6
#define DMA_FRAME_NUM       240

static i2s_chan_handle_t s_tx = NULL; /* speaker, I2S_NUM_0, 24 kHz */
static i2s_chan_handle_t s_rx = NULL; /* mic,     I2S_NUM_1, 16 kHz */

static int s_volume = 70;
static volatile float s_mic_level = 0.0f;
static volatile float s_out_level = 0.0f;

static TaskHandle_t s_mic_task = NULL;
static audio_mic_cb_t s_mic_cb = NULL;
static volatile bool s_mic_running = false;

/* ---------------- I2S setup (mirrors xiaozhi NoAudioCodecSimplex) ---------------- */

static esp_err_t i2s_tx_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = ORBIT_AUDIO_OUTPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = ORBIT_SPK_GPIO_BCLK,
            .ws = ORBIT_SPK_GPIO_LRCK,
            .dout = ORBIT_SPK_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    return i2s_channel_enable(s_tx);
}

static esp_err_t i2s_rx_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = ORBIT_AUDIO_INPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = ORBIT_MIC_GPIO_SCK,
            .ws = ORBIT_MIC_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = ORBIT_MIC_GPIO_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    return i2s_channel_enable(s_rx);
}

/* ---------------- levels ---------------- */

static void update_level(volatile float *level, const int16_t *pcm, size_t n)
{
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)pcm[i] * pcm[i];
    }
    float rms = (n > 0) ? sqrtf((float)(sum / n)) : 0.0f;
    float norm = rms / 6000.0f; /* speech RMS ~1k-6k */
    if (norm > 1.0f) norm = 1.0f;
    *level = *level * 0.75f + norm * 0.25f;
}

/* ---------------- mic task ---------------- */

/* The mic callback runs esp-sr MultiNet detect() on this task's stack, which
 * needs ~28 KB (the original firmware gives its wake-word encode task
 * 4096*7 bytes in PSRAM). The task is created ONCE with a 32 KB PSRAM stack
 * and gated on/off — no per-conversation create/delete, no PSRAM leak. */
#define MIC_TASK_STACK_BYTES (32 * 1024)

static StackType_t *s_mic_stack = NULL;
static StaticTask_t *s_mic_tcb = NULL;
static volatile bool s_mic_active = false;

static void mic_task(void *arg)
{
    (void)arg;
    int32_t *raw = heap_caps_malloc(MIC_FRAMES_PER_READ * sizeof(int32_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(MIC_FRAMES_PER_READ * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw || !pcm) {
        ESP_LOGE(TAG, "mic task: out of memory");
        free(raw); free(pcm);
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    while (1) {
        if (!s_mic_running) {
            s_mic_active = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s_mic_active = true;
        size_t bytes_read = 0;
        if (i2s_channel_read(s_rx, raw, MIC_FRAMES_PER_READ * sizeof(int32_t),
                             &bytes_read, pdMS_TO_TICKS(200)) != ESP_OK) {
            continue;
        }
        size_t n = bytes_read / sizeof(int32_t);
        if (n == 0) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            int32_t v = raw[i] >> 12;
            pcm[i] = (v > INT16_MAX) ? INT16_MAX : (v < -INT16_MAX) ? (int16_t)-INT16_MAX : (int16_t)v;
        }
        update_level(&s_mic_level, pcm, n);
        if (s_mic_cb) {
            s_mic_cb(pcm, n);
        }
    }
}

/* ---------------- public API ---------------- */

esp_err_t audio_pipeline_init(void)
{
    ESP_ERROR_CHECK(i2s_tx_init());
    ESP_ERROR_CHECK(i2s_rx_init());
    ESP_LOGI(TAG, "audio ready: mic 16 kHz (4/5/6) · speaker 24 kHz (7/15/16)");
    return ESP_OK;
}

esp_err_t audio_pipeline_start_mic(audio_mic_cb_t cb)
{
    if (!s_rx) {
        ESP_LOGE(TAG, "start_mic: mic I2S not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    s_mic_cb = cb;
    s_mic_running = true;
    if (!s_mic_task) {
        /* 32 KB PSRAM stack (esp-sr detect needs ~28 KB) + internal TCB */
        s_mic_stack = heap_caps_malloc(MIC_TASK_STACK_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_mic_tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_mic_stack || !s_mic_tcb) {
            ESP_LOGE(TAG, "start_mic: no memory for the mic task stack");
            free(s_mic_stack); free(s_mic_tcb);
            s_mic_stack = NULL; s_mic_tcb = NULL;
            s_mic_running = false;
            return ESP_ERR_NO_MEM;
        }
        s_mic_task = xTaskCreateStatic(mic_task, "mic", MIC_TASK_STACK_BYTES,
                                       NULL, 5, s_mic_stack, s_mic_tcb);
        if (!s_mic_task) {
            ESP_LOGE(TAG, "start_mic: task create failed");
            free(s_mic_stack); free(s_mic_tcb);
            s_mic_stack = NULL; s_mic_tcb = NULL;
            s_mic_running = false;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t audio_pipeline_stop_mic(void)
{
    s_mic_running = false;
    /* the task notices within one I2S read timeout (~200 ms) */
    for (int i = 0; i < 100 && s_mic_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

esp_err_t audio_pipeline_write_spk(const int16_t *pcm24k, size_t frames)
{
    if (!s_tx || frames == 0) {
        return ESP_OK;
    }
    update_level(&s_out_level, pcm24k, frames);

    /* software volume: (v/100)^2 scaling, 16→32 bit */
    int32_t factor = (int32_t)(pow((double)s_volume / 100.0, 2) * 65536.0);
    int32_t *buf = heap_caps_malloc(frames * sizeof(int32_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < frames; i++) {
        int64_t v = (int64_t)pcm24k[i] * factor;
        buf[i] = (v > INT32_MAX) ? INT32_MAX : (v < INT32_MIN) ? INT32_MIN : (int32_t)v;
    }
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_tx, buf, frames * sizeof(int32_t),
                                       &bytes_written, portMAX_DELAY);
    free(buf);
    return err;
}

esp_err_t audio_pipeline_write_spk_16k(const int16_t *pcm16k, size_t frames)
{
    if (frames < 2) {
        return audio_pipeline_write_spk(pcm16k, frames);
    }
    size_t out_frames = frames * 3 / 2;
    int16_t *out = heap_caps_malloc(out_frames * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return ESP_ERR_NO_MEM;
    }
    size_t o = 0;
    for (size_t i = 0; i + 1 < frames; i += 2) {
        int16_t a = pcm16k[i], b = pcm16k[i + 1];
        out[o++] = a;
        out[o++] = (int16_t)((2 * a + b) / 3);
        out[o++] = (int16_t)((a + 2 * b) / 3);
    }
    esp_err_t err = audio_pipeline_write_spk(out, o);
    free(out);
    return err;
}

void audio_pipeline_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    orbit_board_set_volume(volume);
}

int audio_pipeline_get_volume(void)
{
    return s_volume;
}

float audio_pipeline_get_mic_level(void)
{
    return s_mic_level;
}

float audio_pipeline_get_output_level(void)
{
    return s_out_level;
}
