#include "gemini_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mcp_router.h"

static const char *TAG = "gemini";

#define GEMINI_WS_URL  "wss://generativelanguage.googleapis.com/ws/" \
                       "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s"
#define GEMINI_MODEL   "models/gemini-3.8-live"
#define GEMINI_VOICE   "Kore"

#define WS_BUFFER_SIZE   4096
/* the websocket task runs the mbedTLS handshake for wss:// (Gemini Live) —
 * 8 KB was marginal; 12 KB covers handshake + cert-bundle verify */
#define WS_TASK_STACK    12288
#define RX_MSG_MAX       (64 * 1024)  /* reassembled server message (PSRAM) */
#define CONNECT_WAIT_MS  10000

static const char *SYSTEM_INSTRUCTION =
    "You are Orbit, a cheerful voice assistant living on a small ESP32-S3 speaker. "
    "This is a live voice conversation: answer briefly and conversationally — one or two "
    "short sentences unless the user asks for detail. Always reply in the same language "
    "the user speaks. Use the available tools to help: music, alarms, timers, weather, "
    "news, time, volume and any configured remote tools. If a tool fails, say so simply "
    "and move on. Never mention that you are a text model.";

static char s_api_key[128] = {0};
static gemini_client_callbacks_t s_cb;
static volatile bool s_cb_set = false;

static esp_websocket_client_handle_t s_ws = NULL;
static volatile bool s_setup_done = false;   /* setupComplete received */
static volatile bool s_stopping = false;

static uint8_t *s_rx = NULL;  /* message reassembly buffer (PSRAM) */
static int s_rx_len = 0;
static int s_rx_total = 0;

/* ---------------- base64 helpers (mbedtls) ---------------- */

static char *b64_encode(const uint8_t *src, size_t len)
{
    size_t olen = 0;
    if (mbedtls_base64_encode(NULL, 0, &olen, src, len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return NULL;
    }
    char *out = heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return NULL;
    }
    if (mbedtls_base64_encode((unsigned char *)out, olen, &olen, src, len) != 0) {
        free(out);
        return NULL;
    }
    out[olen] = '\0';
    return out;
}

static uint8_t *b64_decode(const char *src, size_t *out_len)
{
    size_t slen = strlen(src);
    size_t olen = 0;
    /* decode into a PSRAM buffer sized for the worst case */
    uint8_t *out = heap_caps_malloc((slen / 4) * 3 + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return NULL;
    }
    if (mbedtls_base64_decode(out, (slen / 4) * 3 + 4, &olen,
                              (const unsigned char *)src, slen) != 0) {
        free(out);
        return NULL;
    }
    *out_len = olen;
    return out;
}

/* ---------------- sending ---------------- */

static void ws_send_json(cJSON *msg)
{
    if (!s_ws || !msg) {
        return;
    }
    char *txt = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!txt) {
        return;
    }
    esp_websocket_client_send_text(s_ws, txt, (int)strlen(txt), pdMS_TO_TICKS(2000));
    free(txt);
}

/* ---------------- setup message ---------------- */

static void send_setup(void)
{
    cJSON *setup = cJSON_CreateObject();

    cJSON *gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *mods = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON_AddItemToArray(mods, cJSON_CreateString("AUDIO"));
    cJSON *speech = cJSON_AddObjectToObject(gen, "speechConfig");
    cJSON *voice = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON *prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", GEMINI_VOICE);
    cJSON_AddNumberToObject(gen, "temperature", 0.7);

    cJSON *sys = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(sys, "parts");
    cJSON *pt = cJSON_CreateObject();
    cJSON_AddStringToObject(pt, "text", SYSTEM_INSTRUCTION);
    cJSON_AddItemToArray(parts, pt);

    /* tools from the MCP router */
    cJSON *tools_arr = gemini_client_tools_json();
    if (tools_arr && cJSON_IsArray(tools_arr) && cJSON_GetArraySize(tools_arr) > 0) {
        cJSON *tools = cJSON_AddArrayToObject(setup, "tools");
        cJSON *decl = cJSON_CreateObject();
        cJSON_AddItemToObject(decl, "functionDeclarations", tools_arr);
        cJSON_AddItemToArray(tools, decl);
    } else if (tools_arr) {
        cJSON_Delete(tools_arr);
    }

    cJSON_AddObjectToObject(setup, "outputAudioTranscription");

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "setup", setup);
    ESP_LOGI(TAG, "sending setup (model %s, voice %s)", GEMINI_MODEL, GEMINI_VOICE);
    ws_send_json(msg);
}

/* ---------------- tool calls (async) ---------------- */

typedef struct {
    cJSON *fns; /* owned */
} tool_call_job_t;

static void tool_call_task(void *arg)
{
    tool_call_job_t *job = arg;
    cJSON *fns = job->fns;
    int n = cJSON_GetArraySize(fns);
    if (n > 4) {
        n = 4; /* router parallelism cap */
    }

    const char *names[4] = {0};
    const cJSON *args[4] = {0};
    const char *ids[4] = {0};
    mcp_result_t results[4];
    memset(results, 0, sizeof(results));

    for (int i = 0; i < n; i++) {
        cJSON *fn = cJSON_GetArrayItem(fns, i);
        cJSON *nm = cJSON_GetObjectItem(fn, "name");
        cJSON *id = cJSON_GetObjectItem(fn, "id");
        names[i] = (cJSON_IsString(nm) && nm->valuestring) ? nm->valuestring : "";
        ids[i] = (cJSON_IsString(id) && id->valuestring) ? id->valuestring : "";
        args[i] = cJSON_GetObjectItem(fn, "args");
        ESP_LOGI(TAG, "tool call: %s(%s)", names[i], ids[i]);
    }

    mcp_router_call_parallel(names, args, (size_t)n, results);

    cJSON *msg = cJSON_CreateObject();
    cJSON *tool_resp = cJSON_AddObjectToObject(msg, "toolResponse");
    cJSON *fr_arr = cJSON_AddArrayToObject(tool_resp, "functionResponses");
    for (int i = 0; i < n; i++) {
        cJSON *fr = cJSON_CreateObject();
        cJSON_AddStringToObject(fr, "id", ids[i]);
        cJSON_AddStringToObject(fr, "name", names[i]);
        cJSON *response = cJSON_AddObjectToObject(fr, "response");
        cJSON *result = cJSON_AddObjectToObject(response, "result");
        cJSON_AddStringToObject(result, "output",
                                results[i].text ? results[i].text : "(no output)");
        cJSON_AddItemToArray(fr_arr, fr);
        mcp_result_free(&results[i]);
    }
    cJSON_Delete(fns);
    free(job);

    if (s_ws && !s_stopping) {
        ws_send_json(msg);
    } else {
        cJSON_Delete(msg);
    }
    vTaskDelete(NULL);
}

static void handle_tool_call(cJSON *tool_call)
{
    cJSON *fns = cJSON_GetObjectItem(tool_call, "functionCalls");
    if (!cJSON_IsArray(fns) || cJSON_GetArraySize(fns) == 0) {
        return;
    }
    tool_call_job_t *job = malloc(sizeof(*job));
    if (!job) {
        return;
    }
    job->fns = cJSON_Duplicate(fns, true); /* deep copy: runs in another task */
    if (!job->fns || xTaskCreate(tool_call_task, "gemini_tool", 6144, job, 4, NULL) != pdPASS) {
        cJSON_Delete(job->fns);
        free(job);
    }
}

/* ---------------- server messages ---------------- */

static void handle_server_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "unparseable server message");
        return;
    }

    if (cJSON_HasObjectItem(root, "setupComplete")) {
        s_setup_done = true;
        ESP_LOGI(TAG, "✅ Gemini Live session ready");
    }

    if (cJSON_HasObjectItem(root, "goAway")) {
        ESP_LOGW(TAG, "server says goAway — session will end");
        s_setup_done = false;
    }

    cJSON *sc = cJSON_GetObjectItem(root, "serverContent");
    if (sc) {
        cJSON *model_turn = cJSON_GetObjectItem(sc, "modelTurn");
        if (model_turn) {
            cJSON *parts = cJSON_GetObjectItem(model_turn, "parts");
            if (cJSON_IsArray(parts)) {
                cJSON *part;
                cJSON_ArrayForEach(part, parts) {
                    cJSON *inl = cJSON_GetObjectItem(part, "inlineData");
                    if (inl) {
                        cJSON *data = cJSON_GetObjectItem(inl, "data");
                        if (cJSON_IsString(data) && data->valuestring[0]) {
                            size_t pcm_len = 0;
                            uint8_t *pcm = b64_decode(data->valuestring, &pcm_len);
                            if (pcm && pcm_len >= 2 && s_cb_set && s_cb.on_audio) {
                                s_cb.on_audio((const int16_t *)pcm, pcm_len / 2);
                            }
                            free(pcm);
                        }
                    }
                    cJSON *text = cJSON_GetObjectItem(part, "text");
                    if (cJSON_IsString(text) && text->valuestring[0] &&
                        s_cb_set && s_cb.on_text) {
                        s_cb.on_text(text->valuestring);
                    }
                }
            }
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "interrupted"))) {
            ESP_LOGI(TAG, "⚡ barge-in (interrupted)");
            if (s_cb_set && s_cb.on_interrupted) {
                s_cb.on_interrupted();
            }
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "turnComplete"))) {
            if (s_cb_set && s_cb.on_turn_complete) {
                s_cb.on_turn_complete();
            }
        }
        cJSON *in_tr = cJSON_GetObjectItem(sc, "inputTranscription");
        if (in_tr) {
            cJSON *t = cJSON_GetObjectItem(in_tr, "text");
            if (cJSON_IsString(t) && t->valuestring[0]) {
                ESP_LOGI(TAG, "🎤 you said: %s", t->valuestring);
                if (s_cb_set && s_cb.on_user_activity) {
                    s_cb.on_user_activity();
                }
            }
        }
        cJSON *out_tr = cJSON_GetObjectItem(sc, "outputTranscription");
        if (out_tr) {
            cJSON *t = cJSON_GetObjectItem(out_tr, "text");
            if (cJSON_IsString(t) && t->valuestring[0] && s_cb_set && s_cb.on_text) {
                ESP_LOGI(TAG, "🗣️ orbit says: %s", t->valuestring);
                s_cb.on_text(t->valuestring);
            }
        }
    }

    cJSON *voice_activity = cJSON_GetObjectItem(root, "voiceActivity");
    if (voice_activity && s_cb_set && s_cb.on_user_activity) {
        s_cb.on_user_activity();
    }

    cJSON *tool_call = cJSON_GetObjectItem(root, "toolCall");
    if (tool_call) {
        handle_tool_call(tool_call);
    }

    cJSON_Delete(root);
}

/* ---------------- websocket events ---------------- */

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected — sending setup");
        s_rx_len = 0;
        s_rx_total = 0;
        send_setup();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (!s_rx || data->payload_len <= 0 || data->op_code != 0x1) {
            break; /* only reassemble text frames */
        }
        if (data->payload_offset == 0) {
            s_rx_len = 0;
            s_rx_total = data->payload_len;
        }
        if (s_rx_len + data->data_len > RX_MSG_MAX) {
            ESP_LOGW(TAG, "server message too large (%d) — dropping", data->payload_len);
            s_rx_len = 0;
            s_rx_total = 0;
            break;
        }
        memcpy(s_rx + s_rx_len, data->data_ptr, (size_t)data->data_len);
        s_rx_len += data->data_len;
        if (s_rx_len >= s_rx_total) {
            s_rx[s_rx_len] = '\0';
            handle_server_message((const char *)s_rx);
            s_rx_len = 0;
            s_rx_total = 0;
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected");
        s_setup_done = false;
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error");
        s_setup_done = false;
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "websocket closed");
        s_setup_done = false;
        break;

    default:
        break;
    }
}

/* ---------------- public API ---------------- */

esp_err_t gemini_client_set_api_key(const char *api_key)
{
    snprintf(s_api_key, sizeof(s_api_key), "%s", api_key ? api_key : "");
    return ESP_OK;
}

esp_err_t gemini_client_set_callbacks(const gemini_client_callbacks_t *cb)
{
    if (cb) {
        s_cb = *cb;
        s_cb_set = true;
    }
    return ESP_OK;
}

esp_err_t gemini_client_connect(void)
{
    if (s_ws) {
        return ESP_OK;
    }
    if (!s_api_key[0]) {
        ESP_LOGE(TAG, "no Gemini API key — set it in the setup portal first");
        return ESP_ERR_INVALID_STATE;
    }
    char url[256];
    snprintf(url, sizeof(url), GEMINI_WS_URL, s_api_key);

    s_rx = heap_caps_malloc(RX_MSG_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx) {
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = WS_BUFFER_SIZE,
        .task_stack = WS_TASK_STACK,
        .task_prio = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 45,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        free(s_rx);
        s_rx = NULL;
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    s_stopping = false;
    s_setup_done = false;
    s_rx_len = 0;
    s_rx_total = 0;

    ESP_LOGI(TAG, "connecting to Gemini Live (%s)…", GEMINI_MODEL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        free(s_rx);
        s_rx = NULL;
        return ESP_FAIL;
    }

    for (int waited = 0; waited < CONNECT_WAIT_MS && !s_setup_done && !s_stopping; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_setup_done) {
        ESP_LOGE(TAG, "Gemini Live setup timed out");
        gemini_client_disconnect();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t gemini_client_send_audio(const uint8_t *pcm16k, size_t len)
{
    if (!s_ws || !s_setup_done || s_stopping || !pcm16k || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    char *b64 = b64_encode(pcm16k, len);
    if (!b64) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "data", b64);
    cJSON_AddStringToObject(audio, "mimeType", "audio/pcm;rate=16000");
    free(b64);

    cJSON *ri = cJSON_CreateObject();
    cJSON_AddItemToObject(ri, "audio", audio);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "realtimeInput", ri);

    char *txt = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    /* real-time audio: never block the mic task for long */
    esp_websocket_client_send_text(s_ws, txt, (int)strlen(txt), pdMS_TO_TICKS(50));
    free(txt);
    return ESP_OK;
}

void gemini_client_disconnect(void)
{
    s_stopping = true;
    s_setup_done = false;
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    free(s_rx);
    s_rx = NULL;
    s_rx_len = 0;
    s_rx_total = 0;
}

bool gemini_client_is_connected(void)
{
    return s_ws != NULL && s_setup_done && !s_stopping;
}

/* ---- MCP bridge ---- */

cJSON *gemini_client_tools_json(void)
{
    return mcp_router_gemini_tools();
}

mcp_result_t gemini_client_route_tool_call(const char *name, const cJSON *args)
{
    return mcp_router_call(name, args);
}
