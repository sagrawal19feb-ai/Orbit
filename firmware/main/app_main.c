/*
 * 🛰️ Orbit — Gemini voice assistant (ESP32-S3)
 *
 * State machine:
 *   IDLE ──(wake word "Orbit" / BOOT button)──▶ CONNECTING ──▶ CONVERSATION
 *   CONVERSATION ──(20 s idle · 5 min max · button)──▶ IDLE
 *   IDLE ──(5 min idle)──▶ deep sleep (RTC alarm wake + BOOT button wake)
 *
 * Alarms/timers ring on top of any state (voice or button to dismiss/snooze).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "orbit_board.h"
#include "assets_reader.h"
#include "settings.h"
#include "live_info.h"
#include "audio_pipeline.h"
#include "orbit_display.h"
#include "wake_word.h"
#include "gemini_client.h"
#include "music_player.h"
#include "alarm_timer.h"
#include "wifi_config.h"
#include "mcp_local.h"
#include "mcp_client.h"
#include "mcp_router.h"

static const char *TAG = "orbit";

#define SESSION_IDLE_TIMEOUT_MS (20 * 1000)      /* end conversation after 20 s of silence */
#define SESSION_MAX_MS          (5 * 60 * 1000)  /* hard session cap */
#define DEEP_SLEEP_IDLE_MS      (5 * 60 * 1000)  /* deep sleep after 5 min idle */
#define RING_TONE_MS            400
#define RING_MAX_MS             (10 * 60 * 1000)

typedef enum { ST_IDLE = 0, ST_CONNECTING, ST_CONVERSATION } orbit_state_t;

static volatile orbit_state_t s_state = ST_IDLE;
static volatile bool s_wake_requested = false;
static volatile bool s_end_session = false;
static volatile bool s_no_key = false;
static volatile bool s_model_speaking = false;
static volatile bool s_alarm_ringing = false;
static volatile bool s_have_gemini_key = false;
static volatile int64_t s_last_activity_us = 0;
static volatile int64_t s_session_start_us = 0;

/* ---------------- helpers ---------------- */

static void play_tone(float freq, int ms)
{
    int total = ORBIT_AUDIO_OUTPUT_SAMPLE_RATE * ms / 1000;
    int16_t buf[256];
    for (int i = 0; i < total; i += 256) {
        int chunk = total - i;
        if (chunk > 256) {
            chunk = 256;
        }
        for (int j = 0; j < chunk; j++) {
            buf[j] = (int16_t)(sinf(2.0f * (float)M_PI * freq * (float)(i + j) /
                                   (float)ORBIT_AUDIO_OUTPUT_SAMPLE_RATE) * 9000.0f);
        }
        audio_pipeline_write_spk(buf, (size_t)chunk);
    }
}

/* ---------------- wake word ---------------- */

static void on_wake_word(void)
{
    /* runs in the mic task — keep it quick */
    s_wake_requested = true;
}

/* ---------------- Gemini callbacks ---------------- */

static void app_mic_cb(const int16_t *pcm, size_t frames)
{
    if (s_state != ST_CONVERSATION) {
        return;
    }
    gemini_client_send_audio((const uint8_t *)pcm, frames * 2);
    if (audio_pipeline_get_mic_level() > 0.02f) {
        s_last_activity_us = esp_timer_get_time(); /* user is speaking */
    }
}

static void gcb_on_audio(const int16_t *pcm, size_t frames)
{
    s_model_speaking = true;
    s_last_activity_us = esp_timer_get_time(); /* model is talking → session alive */
    audio_pipeline_write_spk(pcm, frames);
}

static void gcb_on_turn_complete(void)
{
    s_model_speaking = false;
}

static void gcb_on_interrupted(void)
{
    s_model_speaking = false; /* barge-in: user took over */
}

static void gcb_on_text(const char *text)
{
    (void)text; /* transcriptions are logged by the client */
}

static void gcb_on_user_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* ---------------- session control ---------------- */

static void end_session(void)
{
    gemini_client_disconnect();
    audio_pipeline_stop_mic();
    s_model_speaking = false;
    s_end_session = false;
    gpio_set_level(ORBIT_STATUS_LED_GPIO, 0);
    s_state = ST_IDLE;
    orbit_display_set_state(wifi_config_is_connected() ? ORBIT_DISPLAY_STANDBY
                                                       : ORBIT_DISPLAY_OFFLINE);
    /* re-arm the wake word (restarts the mic with the wake detector) */
    if (wake_word_start(on_wake_word) == ESP_OK) {
        ESP_LOGI(TAG, "listening for \"Orbit\"…");
    }
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "😴 idle for %d min — entering deep sleep "
                  "(RTC alarm wake + BOOT button wake)", DEEP_SLEEP_IDLE_MS / 60000);
    gemini_client_disconnect();
    music_player_stop();
    audio_pipeline_stop_mic();
    wake_word_stop();
    gpio_set_level(ORBIT_STATUS_LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* wake for the next alarm/timer; if the clock isn't synced yet, re-check in 1 h */
    int64_t next_fire = alarm_timer_next_fire_in_seconds();
    uint64_t sleep_us;
    if (next_fire > 0) {
        int64_t cap_s = 24 * 3600;
        int64_t s = (next_fire < cap_s) ? next_fire : cap_s;
        sleep_us = (uint64_t)s * 1000000ULL;
        ESP_LOGI(TAG, "RTC wake in %lld s (next alarm/timer)", (long long)s);
    } else {
        sleep_us = 3600ULL * 1000000ULL;
        ESP_LOGI(TAG, "clock not synced — RTC re-check in 1 h");
    }
    esp_sleep_enable_timer_wakeup(sleep_us);
    /* BOOT button (GPIO0, active-low to GND) */
    esp_sleep_enable_ext1_wakeup(1ULL << ORBIT_WAKE_BUTTON_GPIO, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_deep_sleep_start();
}

/* ---------------- tasks ---------------- */

static void state_task(void *arg)
{
    (void)arg;
    int64_t idle_since_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();

        if (s_state == ST_IDLE) {
            if (s_wake_requested) {
                s_wake_requested = false;
                if (!s_have_gemini_key) {
                    if (!s_no_key) {
                        ESP_LOGW(TAG, "no Gemini API key — join the \"Orbit-Setup\" WiFi and set it at "
                                      "http://192.168.4.1 (or set WiFi first, then the key)");
                    }
                    s_no_key = true;
                    idle_since_us = now;
                    continue;
                }
                s_no_key = false;
                ESP_LOGI(TAG, "🎤 wake — starting a conversation");
                s_state = ST_CONNECTING;
                orbit_display_set_state(ORBIT_DISPLAY_THINKING);
                /* hand the mic from the wake-word detector over to Gemini */
                wake_word_stop();
                audio_pipeline_stop_mic();
                audio_pipeline_start_mic(app_mic_cb);
                if (gemini_client_connect() == ESP_OK) {
                    s_state = ST_CONVERSATION;
                    orbit_display_set_state(ORBIT_DISPLAY_LISTENING);
                    s_session_start_us = now;
                    s_last_activity_us = now;
                    gpio_set_level(ORBIT_STATUS_LED_GPIO, 1);
                    ESP_LOGI(TAG, "🗣️ conversation open — talk to Orbit");
                } else {
                    ESP_LOGE(TAG, "could not reach Gemini — back to idle");
                    end_session();
                }
            } else if (music_player_is_playing() || s_alarm_ringing ||
                       alarm_timer_is_ringing()) {
                idle_since_us = now; /* something is happening — not idle */
            } else if (now - idle_since_us > (int64_t)DEEP_SLEEP_IDLE_MS * 1000) {
                enter_deep_sleep();
            }
        } else if (s_state == ST_CONVERSATION) {
            bool alive = gemini_client_is_connected();
            bool idle = (now - s_last_activity_us) > (int64_t)SESSION_IDLE_TIMEOUT_MS * 1000;
            bool maxed = (now - s_session_start_us) > (int64_t)SESSION_MAX_MS * 1000;
            if (s_end_session || !alive || idle || maxed) {
                ESP_LOGI(TAG, "conversation ended (%s%s%s%s)",
                         s_end_session ? "button " : "", !alive ? "disconnected " : "",
                         idle ? "idle " : "", maxed ? "time cap " : "");
                end_session();
                idle_since_us = esp_timer_get_time();
            }
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    bool was = false;
    while (1) {
        bool pressed = orbit_board_wake_button_pressed();
        if (pressed && !was) {
            if (alarm_timer_is_ringing()) {
                ESP_LOGI(TAG, "button: dismiss alarm");
                alarm_timer_dismiss();
            } else if (s_state == ST_IDLE) {
                ESP_LOGI(TAG, "button: wake");
                s_wake_requested = true;
            } else if (s_state == ST_CONVERSATION) {
                ESP_LOGI(TAG, "button: end conversation");
                s_end_session = true;
            }
        }
        was = pressed;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void on_alarm_fire(int id)
{
    (void)id;
    s_alarm_ringing = true;
}

static void ring_task(void *arg)
{
    (void)arg;
    while (1) {
        if (!s_alarm_ringing) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(TAG, "⏰ alarm is ringing (button or \"stop alarm\" to dismiss)");
        int64_t ring_start = esp_timer_get_time();
        bool led = false;
        while (alarm_timer_is_ringing()) {
            play_tone(880.0f, RING_TONE_MS);   /* two-tone ring */
            if (!alarm_timer_is_ringing()) {
                break;
            }
            play_tone(1320.0f, RING_TONE_MS);
            led = !led;
            gpio_set_level(ORBIT_STATUS_LED_GPIO, led ? 1 : 0);
            if (esp_timer_get_time() - ring_start > (int64_t)RING_MAX_MS * 1000) {
                ESP_LOGW(TAG, "alarm rang for 10 min — auto-dismissing");
                alarm_timer_dismiss();
                break;
            }
        }
        s_alarm_ringing = false;
        gpio_set_level(ORBIT_STATUS_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void display_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        char *t = live_info_time(); /* "HH:MM" or "--:--" */
        if (t) {
            int h = -1, m = -1;
            if (sscanf(t, "%d:%d", &h, &m) == 2) {
                orbit_display_set_time(h, m);
            } else {
                orbit_display_set_time(-1, -1);
            }
            free(t);
        }

        orbit_display_state_t st;
        if (alarm_timer_is_ringing() || s_alarm_ringing) {
            st = ORBIT_DISPLAY_ALARM;
        } else if (music_player_is_playing()) {
            st = ORBIT_DISPLAY_PLAYING;
        } else if (s_state == ST_CONVERSATION) {
            st = s_model_speaking ? ORBIT_DISPLAY_SPEAKING : ORBIT_DISPLAY_LISTENING;
        } else if (s_state == ST_CONNECTING) {
            st = ORBIT_DISPLAY_THINKING;
        } else if (s_no_key || !wifi_config_is_connected()) {
            st = ORBIT_DISPLAY_OFFLINE;
        } else {
            st = ORBIT_DISPLAY_STANDBY;
        }
        orbit_display_set_state(st);
    }
}

/* ---------------- hardware test (hold BOOT at power-on) ---------------- */

static volatile float s_hw_mic_level = 0.0f;

static void hw_test_mic_cb(const int16_t *pcm, size_t frames)
{
    double sum = 0;
    for (size_t i = 0; i < frames; i++) {
        sum += (double)pcm[i] * pcm[i];
    }
    float rms = (frames > 0) ? sqrtf((float)(sum / (double)frames)) : 0.0f;
    float norm = rms / 6000.0f;
    if (norm > 1.0f) norm = 1.0f;
    s_hw_mic_level = s_hw_mic_level * 0.7f + norm * 0.3f;
}

static void hardware_test(void)
{
    ESP_LOGW(TAG, "🔧 HARDWARE TEST — speaker: 3 tones · display: 4 patterns · mic: 6 s");
    orbit_display_test_mode(true);

    /* speaker */
    play_tone(440.0f, 300);
    play_tone(880.0f, 300);
    play_tone(1320.0f, 300);

    /* display: clear / border+HW TEST / checkerboard / full */
    for (int phase = 0; phase < 4; phase++) {
        orbit_display_test_pattern((uint8_t)phase);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* mic: 6 s loopback — LED + on-screen bar + log */
    s_hw_mic_level = 0.0f;
    if (audio_pipeline_start_mic(hw_test_mic_cb) == ESP_OK) {
        int64_t start = esp_timer_get_time();
        while (esp_timer_get_time() - start < 6LL * 1000 * 1000) {
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(ORBIT_STATUS_LED_GPIO, s_hw_mic_level > 0.05f ? 1 : 0);
            orbit_display_set_audio_level(s_hw_mic_level);
            orbit_display_test_pattern(4);
            ESP_LOGI(TAG, "🔧 mic level %.3f — speak into the mic", (double)s_hw_mic_level);
        }
        audio_pipeline_stop_mic();
        gpio_set_level(ORBIT_STATUS_LED_GPIO, 0);
    } else {
        ESP_LOGE(TAG, "🔧 mic start failed — check the I2S mic wiring");
    }
    orbit_display_set_audio_level(-1.0f);
    orbit_display_test_pattern(0);
    orbit_display_test_mode(false);
    ESP_LOGW(TAG, "🔧 hardware test done — continuing normal boot");
}

/* ---------------- boot ---------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "🛰️  Orbit assistant starting…");
    ESP_LOGI(TAG, "hold BOOT during power-on for the hardware test");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Chip: ESP32-S3 (rev %d), %d core(s), flash: %lu MB",
             chip_info.revision, chip_info.cores, (unsigned long)(flash_size / (1024 * 1024)));

    /* NVS (settings, alarms, MCP server config) — fatal if broken */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "init: nvs ok");

    /* Board hardware: same pins as the original firmware (no lamp, 1 wake button).
     * From here on, peripheral failures are logged and boot CONTINUES — a missing
     * OLED or mic must never brick the boot (that was the boot-loop bug). */
    if (orbit_board_init() != ESP_OK) {
        ESP_LOGE(TAG, "init: board FAILED (LED/button may not work) — continuing");
    }

    /* Assets partition: wake-word model (mn6_en "Orbit") */
    if (assets_reader_init() != ESP_OK) {
        ESP_LOGW(TAG, "init: assets FAILED — wake word may not load");
    }
    size_t sz = 0;
    if (assets_get_file("srmodels.bin", &sz) != NULL) {
        ESP_LOGI(TAG, "wake-word model found: srmodels.bin (%u bytes)", (unsigned)sz);
    } else {
        ESP_LOGE(TAG, "srmodels.bin NOT found in the assets partition — "
                      "flash assets.bin at 0x420000");
    }

    /* Audio pipeline (mic + speaker) + boot beep */
    if (audio_pipeline_init() != ESP_OK) {
        ESP_LOGE(TAG, "init: audio FAILED — no mic/speaker");
    } else {
        audio_pipeline_set_volume(orbit_board_get_volume());
        ESP_LOGI(TAG, "init: audio ok");
        play_tone(880.0f, 120);  /* boot beep: the speaker works */
        play_tone(1320.0f, 120);
    }

    /* Display (0.91" OLED) */
    if (orbit_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "init: display FAILED — continuing without OLED");
    }

    /* Hardware test: hold BOOT during power-on (~20 s) */
    if (orbit_board_wake_button_pressed()) {
        hardware_test();
    }

    /* Settings, then WiFi (station, or the Orbit-Setup AP + portal) */
    if (settings_init() != ESP_OK) {
        ESP_LOGE(TAG, "init: settings FAILED — continuing with defaults");
    }
    if (wifi_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "init: wifi FAILED — offline until fixed");
    }
    if (live_info_init() != ESP_OK) {
        ESP_LOGW(TAG, "init: live info FAILED — no weather/news/time sync");
    }

    /* Alarms & timers (NVS-persisted, SNTP-accurate) */
    if (alarm_timer_init() != ESP_OK) {
        ESP_LOGE(TAG, "init: alarms FAILED");
    } else {
        alarm_timer_set_fire_callback(on_alarm_fire);
    }

    /* Wake word (ESP-SR MultiNet6 "Orbit" from the assets partition) */
    if (wake_word_init() != ESP_OK) {
        ESP_LOGE(TAG, "wake-word model failed to load — only the BOOT button will wake Orbit");
    }

    /* MCP tool pipeline: local device tools + remote MCP servers */
    if (mcp_local_init() != ESP_OK || mcp_client_init() != ESP_OK ||
        mcp_router_init() != ESP_OK) {
        ESP_LOGE(TAG, "init: MCP pipeline FAILED — Gemini gets no tools");
    } else {
        /* MCP self-test: the tool list Gemini will see + one local tool */
        cJSON *tools = mcp_router_gemini_tools();
        ESP_LOGI(TAG, "MCP pipeline ready: %d tools exposed to Gemini", cJSON_GetArraySize(tools));
        cJSON_Delete(tools);
        mcp_result_t r = mcp_router_call("get_time", NULL);
        ESP_LOGI(TAG, "MCP self-test get_time -> %s", r.text ? r.text : "(null)");
        mcp_result_free(&r);
    }

    /* Gemini Live client */
    char key[128] = {0};
    settings_get_gemini_api_key(key, sizeof(key));
    s_have_gemini_key = key[0] != '\0';
    gemini_client_set_api_key(key);
    gemini_client_callbacks_t cb = {
        .on_text = gcb_on_text,
        .on_audio = gcb_on_audio,
        .on_turn_complete = gcb_on_turn_complete,
        .on_interrupted = gcb_on_interrupted,
        .on_user_activity = gcb_on_user_activity,
    };
    gemini_client_set_callbacks(&cb);
    if (!s_have_gemini_key) {
        ESP_LOGW(TAG, "no Gemini API key yet — set it from the setup portal");
        s_no_key = true;
    }

    /* arm the wake word (starts the mic) */
    if (wake_word_start(on_wake_word) == ESP_OK) {
        ESP_LOGI(TAG, "listening for \"Orbit\"…");
    }

    /* tasks */
    xTaskCreate(state_task, "orbit_state", 8192, NULL, 4, NULL);
    xTaskCreate(button_task, "orbit_btn", 3072, NULL, 4, NULL);
    xTaskCreate(ring_task, "orbit_ring", 4096, NULL, 4, NULL);
    xTaskCreate(display_task, "orbit_disp", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "✅ Orbit is ready — say \"Orbit\" or press BOOT · deep sleep after 5 min idle");
}
