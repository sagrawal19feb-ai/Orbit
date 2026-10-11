#pragma once

#include "esp_err.h"
#include "mcp_common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Gemini Live API client — WebSocket (wss://generativelanguage.googleapis.com),
 * model gemini-3.8-live, native audio (16 kHz PCM in, 24 kHz PCM out).
 * Tools come from the MCP router (function calling bridge).
 */

/* callbacks invoked from the WebSocket task (keep them quick/non-blocking
 * except on_audio, which plays and is naturally paced by the I2S write) */
typedef struct {
    void (*on_text)(const char *text);                    /* model text / transcription */
    void (*on_audio)(const int16_t *pcm, size_t frames);  /* 24 kHz PCM to play */
    void (*on_turn_complete)(void);                       /* model finished its turn */
    void (*on_interrupted)(void);                         /* barge-in: stop playback */
    void (*on_user_activity)(void);                       /* user spoke (VAD/transcript) */
} gemini_client_callbacks_t;

esp_err_t gemini_client_set_api_key(const char *api_key);
esp_err_t gemini_client_set_callbacks(const gemini_client_callbacks_t *cb);

esp_err_t gemini_client_connect(void);
esp_err_t gemini_client_send_audio(const uint8_t *pcm16k, size_t len);
void gemini_client_disconnect(void);
bool gemini_client_is_connected(void);

/* ---- MCP bridge ---- */
cJSON *gemini_client_tools_json(void);          /* FunctionDeclarations for setup */
mcp_result_t gemini_client_route_tool_call(const char *name, const cJSON *args);
