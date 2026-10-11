#include "music_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "minimp3.h"
#include "audio_pipeline.h"
#include "settings.h"

static const char *TAG = "music";

#define HTTP_BUF_BYTES   16384
#define PCM_MAX_SAMPLES  MINIMP3_MAX_SAMPLES_PER_FRAME /* int16 slots (all channels) */
#define OUT_MAX_SAMPLES  (PCM_MAX_SAMPLES * 2 + 64)
/* bytes (IDF xTaskCreate depth is in BYTES, not words): minimp3 keeps a
 * ~17 KB scratch on the decode path and mbedTLS handshakes on the same task
 * for HTTPS streams — 32 KB in PSRAM covers both */
#define STREAM_TASK_STACK (32768)

typedef enum { MP_IDLE = 0, MP_PLAYING, MP_PAUSED } mp_state_t;

static volatile mp_state_t s_state = MP_IDLE;
static TaskHandle_t s_task = NULL;
static char s_url[512] = {0};
static volatile bool s_stop = false;

/* ---------------- URL-encoding ---------------- */

static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_len; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0xF];
        }
    }
    out[o] = '\0';
}

/* ---------------- HTTP GET (JSON APIs) ---------------- */

static char *http_get(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .buffer_size = 8192,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        return NULL;
    }
    char *body = heap_caps_malloc(24576, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        esp_http_client_cleanup(http);
        return NULL;
    }
    size_t total = 0;
    esp_err_t err = esp_http_client_open(http, 0);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(http);
        if (status == 200) {
            int r;
            while (total < 24000 &&
                   (r = esp_http_client_read(http, body + total, 24576 - 1 - total)) > 0) {
                total += (size_t)r;
            }
            body[total] = '\0';
        } else {
            ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
            total = 0;
            body[0] = '\0';
        }
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        total = 0;
        body[0] = '\0';
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    if (out_len) {
        *out_len = total;
    }
    return body;
}

/* ---------------- track search ---------------- */

/* Jamendo (full songs, free client ID from settings). Returns a malloc'd
 * stream URL or NULL. */
static char *search_jamendo(const char *query)
{
    char jid[64] = {0};
    settings_get_jamendo_client_id(jid, sizeof(jid));
    if (!jid[0]) {
        ESP_LOGW(TAG, "no Jamendo client ID configured");
        return NULL;
    }
    char enc[256];
    url_encode(query, enc, sizeof(enc));
    char url[640];
    snprintf(url, sizeof(url),
             "https://api.jamendo.com/v3.0/tracks/?client_id=%s&format=json&limit=3"
             "&name=%s&order=popularity_total&audioformat=mp31",
             jid, enc);
    char *body = http_get(url, NULL);
    if (!body) {
        return NULL;
    }
    char *result = NULL;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *results = cJSON_GetObjectItem(root, "results");
        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
            cJSON *t0 = cJSON_GetArrayItem(results, 0);
            cJSON *name = cJSON_GetObjectItem(t0, "name");
            cJSON *artist = cJSON_GetObjectItem(t0, "artist_name");
            cJSON *audio = cJSON_GetObjectItem(t0, "audio");
            if (cJSON_IsString(audio) && audio->valuestring[0]) {
                result = strdup(audio->valuestring);
                ESP_LOGI(TAG, "Jamendo: \"%s\" by %s",
                         (cJSON_IsString(name) && name->valuestring) ? name->valuestring : "?",
                         (cJSON_IsString(artist) && artist->valuestring) ? artist->valuestring : "?");
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    return result;
}

/* Deezer (30-second previews, no key). Returns a malloc'd URL or NULL. */
static char *search_deezer(const char *query)
{
    char enc[256];
    url_encode(query, enc, sizeof(enc));
    char url[512];
    snprintf(url, sizeof(url), "https://api.deezer.com/search?q=%s&limit=1", enc);
    char *body = http_get(url, NULL);
    if (!body) {
        return NULL;
    }
    char *result = NULL;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
            cJSON *t0 = cJSON_GetArrayItem(data, 0);
            cJSON *title = cJSON_GetObjectItem(t0, "title");
            cJSON *artist = cJSON_GetObjectItem(t0, "artist");
            cJSON *cname = artist ? cJSON_GetObjectItem(artist, "name") : NULL;
            cJSON *preview = cJSON_GetObjectItem(t0, "preview");
            if (cJSON_IsString(preview) && preview->valuestring[0]) {
                result = strdup(preview->valuestring);
                ESP_LOGI(TAG, "Deezer (30 s preview): \"%s\" by %s",
                         (cJSON_IsString(title) && title->valuestring) ? title->valuestring : "?",
                         (cJSON_IsString(cname) && cname->valuestring) ? cname->valuestring : "?");
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    return result;
}

/* ---------------- 44.1k(ish) → 24 kHz mono resampler (linear) ---------------- */

typedef struct {
    double   step;       /* source samples per output sample */
    double   next_p;     /* global source position of the next output sample */
    int64_t  g0;         /* global index of the current frame's first sample */
    int16_t  hist;       /* sample at global index g0-1 (frame boundary) */
    bool     started;
} rs24_t;

static int samples_per_channel(const mp3dec_frame_info_t *info)
{
    if (info->layer == 1) {
        return 384;
    }
    if (info->layer == 2) {
        return 1152;
    }
    /* layer 3: MPEG2/2.5 (<= 24 kHz) uses 576-sample frames */
    return (info->hz <= 24000) ? 576 : 1152;
}

static int resample_frame(rs24_t *rs, int src_hz, const int16_t *mono, int n,
                          int16_t *out, int out_cap)
{
    if (n <= 0 || src_hz <= 0) {
        return 0;
    }
    if (!rs->started) {
        rs->step = (double)src_hz / 24000.0;
        rs->next_p = 0;
        rs->g0 = 0;
        rs->hist = mono[0];
        rs->started = true;
    }
    int out_n = 0;
    double p = rs->next_p;
    int64_t g0 = rs->g0;
    /* we can interpolate while both neighbours are inside this frame */
    while (p < (double)(g0 + n - 1) && out_n < out_cap) {
        int64_t gi = (int64_t)p;
        double frac = p - (double)gi;
        int idx = (int)(gi - g0); /* may be -1 at a frame boundary → use hist */
        int16_t s0 = (idx < 0) ? rs->hist : mono[idx];
        int16_t s1 = mono[idx + 1];
        out[out_n++] = (int16_t)(s0 + (int16_t)(((float)(s1 - s0)) * (float)frac));
        p += rs->step;
    }
    rs->next_p = p;
    rs->g0 = g0 + n;
    rs->hist = mono[n - 1];
    return out_n;
}

/* ---------------- streaming task ---------------- */

static void stream_task(void *arg)
{
    ESP_LOGI(TAG, "streaming %s", s_url);

    esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = 15000,
        .buffer_size = 16384,
        .buffer_size_tx = 2048,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    uint8_t *buf = heap_caps_malloc(HTTP_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(PCM_MAX_SAMPLES * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono = heap_caps_malloc(PCM_MAX_SAMPLES * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *out = heap_caps_malloc(OUT_MAX_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    bool ok = false;
    if (http && buf && pcm && mono && out) {
        if (esp_http_client_open(http, 0) == ESP_OK) {
            int status = esp_http_client_get_status_code(http);
            if (status == 200) {
                ok = true;
            } else {
                ESP_LOGW(TAG, "stream HTTP %d", status);
            }
        } else {
            ESP_LOGW(TAG, "stream open failed");
        }
    }

    if (ok) {
        mp3dec_t dec;
        mp3dec_init(&dec);
        rs24_t rs;
        memset(&rs, 0, sizeof(rs));
        size_t have = 0;
        bool eof = false;

        while (!s_stop && !eof) {
            if (s_state == MP_PAUSED) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            /* keep the buffer topped up */
            if (have < HTTP_BUF_BYTES - 4096) {
                int r = esp_http_client_read(http, (char *)buf + have,
                                             HTTP_BUF_BYTES - have);
                if (r > 0) {
                    have += (size_t)r;
                    continue;
                } else if (r == 0) {
                    eof = true; /* stream finished */
                } else {
                    ESP_LOGW(TAG, "stream read error");
                    break;
                }
            }
            if (have == 0) {
                continue;
            }
            /* decode as much as possible from the front of the buffer */
            size_t off = 0;
            while (off < have && !s_stop) {
                mp3dec_frame_info_t info;
                int consumed = mp3dec_decode_frame(&dec, buf + off, (int)(have - off),
                                                   pcm, &info);
                if (consumed < 0) {
                    off = have; /* unrecoverable decode error: drop the rest */
                    break;
                }
                if (consumed == 0) {
                    /* needs more data; skip any garbage bytes it found */
                    off += (size_t)info.frame_bytes;
                    break;
                }
                off += (size_t)consumed;
                if (info.frame_bytes > 0 && info.channels > 0) {
                    int spc = samples_per_channel(&info);
                    int total = spc * info.channels;
                    if (total > PCM_MAX_SAMPLES) {
                        total = PCM_MAX_SAMPLES;
                    }
                    if (info.channels == 2) {
                        for (int i = 0; i < spc; i++) {
                            mono[i] = (int16_t)(((int)pcm[2 * i] + pcm[2 * i + 1]) / 2);
                        }
                    } else {
                        memcpy(mono, pcm, (size_t)spc * sizeof(int16_t));
                    }
                    int out_n = resample_frame(&rs, info.hz, mono, spc,
                                               out, OUT_MAX_SAMPLES);
                    if (out_n > 0) {
                        audio_pipeline_write_spk(out, (size_t)out_n);
                    }
                }
            }
            if (off > 0) {
                memmove(buf, buf + off, have - off);
                have -= off;
            }
        }
        ESP_LOGI(TAG, "stream finished%s", s_stop ? " (stopped)" : " (end of track)");
    }

    if (http) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    free(buf);
    free(pcm);
    free(mono);
    free(out);

    s_state = MP_IDLE;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* start streaming s_url in the background */
static esp_err_t start_stream(const char *url)
{
    music_player_stop();
    snprintf(s_url, sizeof(s_url), "%s", url);
    s_stop = false;
    s_state = MP_PLAYING;
    if (xTaskCreateWithCaps(stream_task, "music_stream", STREAM_TASK_STACK, NULL, 5,
                            &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_state = MP_IDLE;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------- public API ---------------- */

esp_err_t music_player_init(void)
{
    ESP_LOGI(TAG, "music player ready (Jamendo full songs · Deezer 30 s previews · direct URLs)");
    return ESP_OK;
}

esp_err_t music_player_play_query(const char *query)
{
    if (!query || !query[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    /* a direct URL? */
    if (strncmp(query, "http://", 7) == 0 || strncmp(query, "https://", 8) == 0) {
        return music_player_play_url(query);
    }
    char *url = search_jamendo(query);
    if (!url) {
        ESP_LOGW(TAG, "Jamendo found nothing — trying Deezer previews");
        url = search_deezer(query);
    }
    if (!url) {
        ESP_LOGE(TAG, "no track found for \"%s\"", query);
        return ESP_FAIL;
    }
    esp_err_t err = start_stream(url);
    free(url);
    return err;
}

esp_err_t music_player_play_url(const char *url)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "play URL: %s", url);
    return start_stream(url);
}

void music_player_stop(void)
{
    if (s_task) {
        s_stop = true;
        for (int i = 0; i < 40 && s_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    s_state = MP_IDLE;
}

void music_player_pause(void)
{
    if (s_state == MP_PLAYING) {
        s_state = MP_PAUSED;
    }
}

void music_player_resume(void)
{
    if (s_state == MP_PAUSED) {
        s_state = MP_PLAYING;
    }
}

bool music_player_is_playing(void)
{
    return s_state != MP_IDLE;
}
