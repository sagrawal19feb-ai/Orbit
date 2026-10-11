#include "wake_word.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "model_path.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

#include "audio_pipeline.h"

static const char *TAG = "wake";

#define WAKE_WORD        "Orbit"
#define WAKE_DURATION_MS 3000
#define WAKE_THRESHOLD   0.5f

static srmodel_list_t *s_models = NULL;
static esp_mn_iface_t *s_mn = NULL;
static model_iface_data_t *s_mn_data = NULL;
static int s_chunksize = 0;
static int16_t *s_chunk_buf = NULL; /* PSRAM */
static size_t s_chunk_fill = 0;

static wake_word_cb_t s_on_wake = NULL;
static volatile bool s_running = false;

esp_err_t wake_word_init(void)
{
    /* The assets partition holds the raw esp-sr srmodels.bin (mn6_en "Orbit") */
    s_models = esp_srmodel_init("assets");
    if (!s_models || s_models->num == -1) {
        ESP_LOGE(TAG, "failed to load srmodels from the assets partition — "
                      "flash assets.bin at 0x800000");
        return ESP_FAIL;
    }

    /* find the English multinet model */
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, "en");
    if (!mn_name) {
        mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, NULL);
    }
    if (!mn_name) {
        ESP_LOGE(TAG, "no multinet model found in srmodels.bin");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wake-word model: %s", mn_name);

    s_mn = esp_mn_handle_from_name(mn_name);
    if (!s_mn) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name failed");
        return ESP_FAIL;
    }
    s_mn_data = s_mn->create(mn_name, WAKE_DURATION_MS);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "multinet create failed");
        return ESP_FAIL;
    }
    s_mn->set_det_threshold(s_mn_data, WAKE_THRESHOLD);
    s_chunksize = s_mn->get_samp_chunksize(s_mn_data);
    s_chunk_buf = heap_caps_malloc((size_t)s_chunksize * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chunk_buf) {
        return ESP_ERR_NO_MEM;
    }

    /* register the wake command */
    esp_mn_commands_clear();
    esp_mn_commands_add(1, WAKE_WORD);
    esp_mn_commands_update();
    s_mn->print_active_speech_commands(s_mn_data);

    ESP_LOGI(TAG, "wake word ready: \"%s\" (%d Hz, chunk %d samples)",
             WAKE_WORD, s_mn->get_samp_rate(s_mn_data), s_chunksize);
    return ESP_OK;
}

static void feed_detect(const int16_t *pcm, size_t frames)
{
    if (!s_running || !s_mn_data) {
        return;
    }
    for (size_t i = 0; i < frames; i++) {
        s_chunk_buf[s_chunk_fill++] = pcm[i];
        if (s_chunk_fill >= (size_t)s_chunksize) {
            esp_mn_state_t state = s_mn->detect(s_mn_data, s_chunk_buf);
            s_chunk_fill = 0;
            if (state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *res = s_mn->get_results(s_mn_data);
                ESP_LOGI(TAG, "🎤 WAKE WORD detected: \"%s\" (id=%d, prob=%.2f)",
                         res->string, res->command_id[0], res->prob[0]);
                s_running = false; /* stop feeding; the mic keeps running until stopped */
                if (s_on_wake) {
                    s_on_wake();
                }
                return;
            }
        }
    }
}

esp_err_t wake_word_start(wake_word_cb_t on_wake)
{
    if (!s_mn_data) {
        return ESP_FAIL;
    }
    s_on_wake = on_wake;
    s_chunk_fill = 0;
    s_running = true;
    return audio_pipeline_start_mic(feed_detect);
}

void wake_word_stop(void)
{
    s_running = false;
}
