#include "mcp_local.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "music_player.h"
#include "alarm_timer.h"
#include "audio_pipeline.h"
#include "orbit_board.h"
#include "live_info.h"
#include "settings.h"

static const char *TAG = "mcp_local";

/* ---------------- helpers ---------------- */

static char *fmt(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    char stackbuf[256];
    int n = vsnprintf(stackbuf, sizeof(stackbuf), format, ap);
    va_end(ap);
    if (n < 0) {
        return NULL;
    }
    size_t need = (size_t)n + 1;
    char *out = heap_caps_malloc(need > sizeof(stackbuf) ? need : sizeof(stackbuf),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return NULL;
    }
    if (need <= sizeof(stackbuf)) {
        memcpy(out, stackbuf, need);
    } else {
        va_start(ap, format);
        vsnprintf(out, need, format, ap);
        va_end(ap);
    }
    return out;
}

/* tool handler: receives arguments JSON, returns malloc'd result text */
typedef char *(*mcp_tool_handler_t)(const cJSON *args);

/* ---------------- tool handlers ---------------- */

static char *h_play_song(const cJSON *args)
{
    const cJSON *q = cJSON_GetObjectItem(args, "query");
    if (!q || !cJSON_IsString(q) || !q->valuestring[0]) {
        return fmt("Missing 'query' — tell me what to play.");
    }
    music_player_play_query(q->valuestring);
    return fmt("Playing %s.", q->valuestring);
}

static char *h_stop_music(const cJSON *args)
{
    (void)args;
    music_player_stop();
    return fmt("Music stopped.");
}

static char *h_pause_music(const cJSON *args)
{
    (void)args;
    music_player_pause();
    return fmt("Music paused.");
}

static char *h_resume_music(const cJSON *args)
{
    (void)args;
    music_player_resume();
    return fmt("Music resumed.");
}

static char *h_set_alarm(const cJSON *args)
{
    const cJSON *t = cJSON_GetObjectItem(args, "time");
    if (!t || !cJSON_IsString(t)) {
        return fmt("Missing 'time' — use 24-hour HH:MM, like 07:30.");
    }
    int h, m;
    if (sscanf(t->valuestring, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return fmt("I couldn't read the time %s — use 24-hour HH:MM, like 07:30.", t->valuestring);
    }
    int id = alarm_timer_add_alarm(h, m);
    if (id < 0) {
        return fmt("Sorry, I couldn't set that alarm.");
    }
    return fmt("Alarm set for %02d:%02d.", h, m);
}

static char *h_cancel_alarm(const cJSON *args)
{
    const cJSON *id = cJSON_GetObjectItem(args, "id");
    if (!id || !cJSON_IsNumber(id)) {
        return fmt("Missing alarm 'id'.");
    }
    alarm_timer_cancel(id->valueint);
    return fmt("Alarm %d cancelled.", id->valueint);
}

static char *h_stop_alarm(const cJSON *args)
{
    (void)args;
    if (!alarm_timer_is_ringing()) {
        return fmt("No alarm is ringing right now.");
    }
    alarm_timer_dismiss();
    return fmt("Alarm stopped.");
}

static char *h_snooze_alarm(const cJSON *args)
{
    (void)args;
    if (!alarm_timer_is_ringing()) {
        return fmt("No alarm is ringing right now.");
    }
    alarm_timer_snooze();
    return fmt("Alarm snoozed for 5 minutes.");
}

static char *h_set_timer(const cJSON *args)
{
    const cJSON *s = cJSON_GetObjectItem(args, "seconds");
    if (!s || !cJSON_IsNumber(s) || s->valueint <= 0) {
        return fmt("Missing 'seconds' — how long should the timer be?");
    }
    int id = alarm_timer_add_timer(s->valueint);
    if (id < 0) {
        return fmt("Sorry, I couldn't set that timer.");
    }
    int mins = s->valueint / 60;
    if (mins > 0) {
        return fmt("Timer set for %d minutes.", mins);
    }
    return fmt("Timer set for %d seconds.", s->valueint);
}

static char *h_list_alarms(const cJSON *args)
{
    (void)args;
    int n = alarm_timer_count();
    if (n <= 0) {
        return fmt("You have no alarms or timers set.");
    }
    size_t cap = 64;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        return NULL;
    }
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        orbit_alarm_t a = alarm_timer_get(i);
        char line[96];
        if (a.is_timer) {
            snprintf(line, sizeof(line), "%d. timer, %d seconds, %s\n", a.id, a.seconds,
                     a.enabled ? "on" : "off");
        } else {
            snprintf(line, sizeof(line), "%d. alarm %02d:%02d, %s\n", a.id, a.hour, a.minute,
                     a.enabled ? "on" : "off");
        }
        size_t need = strlen(out) + strlen(line) + 1;
        if (need > cap) {
            cap = need * 2;
            char *nb = heap_caps_realloc(out, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                free(out);
                return NULL;
            }
            out = nb;
        }
        strcat(out, line);
    }
    return out;
}

static char *h_set_volume(const cJSON *args)
{
    const cJSON *v = cJSON_GetObjectItem(args, "volume");
    if (!v || !cJSON_IsNumber(v)) {
        return fmt("Missing 'volume' (0-100).");
    }
    int vol = v->valueint;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    audio_pipeline_set_volume(vol);
    orbit_board_set_volume(vol);
    return fmt("Volume set to %d.", vol);
}

static char *h_get_weather(const cJSON *args)
{
    const cJSON *city = cJSON_GetObjectItem(args, "city");
    const char *c = (city && cJSON_IsString(city) && city->valuestring[0]) ? city->valuestring : NULL;
    char *text = live_info_weather(c);
    return text ? text : fmt("Sorry, I couldn't get the weather.");
}

static char *h_get_news(const cJSON *args)
{
    const cJSON *topic = cJSON_GetObjectItem(args, "topic");
    const char *t = (topic && cJSON_IsString(topic) && topic->valuestring[0]) ? topic->valuestring : NULL;
    char *text = live_info_news(t);
    return text ? text : fmt("Sorry, I couldn't get the news.");
}

static char *h_get_time(const cJSON *args)
{
    (void)args;
    char *t = live_info_time();
    if (!t) {
        return fmt("I don't know the time yet.");
    }
    char *out = fmt("The time is %s.", t);
    free(t);
    return out;
}

static char *h_set_city(const cJSON *args)
{
    const cJSON *city = cJSON_GetObjectItem(args, "city");
    if (!city || !cJSON_IsString(city) || !city->valuestring[0]) {
        return fmt("Which city?");
    }
    settings_set_city(city->valuestring);
    return fmt("Got it — I'll remember you're in %s.", city->valuestring);
}

/* ---------------- tool table ---------------- */

#define SCHEMA_OBJ(...) "{\"type\":\"object\",\"properties\":{__VA_ARGS__}}"

typedef struct {
    const char *name;
    const char *desc;
    const char *schema_json;
    mcp_tool_handler_t handler;
} local_tool_def_t;

static const local_tool_def_t s_tools[] = {
    { "play_song", "Play a song or artist by search, or stream a direct URL",
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Song, artist or URL to play\"}},\"required\":[\"query\"]}",
      h_play_song },
    { "stop_music", "Stop music playback", SCHEMA_OBJ(""), h_stop_music },
    { "pause_music", "Pause music playback", SCHEMA_OBJ(""), h_pause_music },
    { "resume_music", "Resume music playback", SCHEMA_OBJ(""), h_resume_music },
    { "set_alarm", "Set a daily alarm",
      "{\"type\":\"object\",\"properties\":{\"time\":{\"type\":\"string\",\"description\":\"Alarm time in 24-hour HH:MM format, e.g. 07:30\"}},\"required\":[\"time\"]}",
      h_set_alarm },
    { "cancel_alarm", "Cancel an alarm or timer by id",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Alarm id from list_alarms\"}},\"required\":[\"id\"]}",
      h_cancel_alarm },
    { "stop_alarm", "Stop the alarm that is currently ringing", SCHEMA_OBJ(""), h_stop_alarm },
    { "snooze_alarm", "Snooze the ringing alarm by 5 minutes", SCHEMA_OBJ(""), h_snooze_alarm },
    { "set_timer", "Set a one-shot countdown timer",
      "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"integer\",\"description\":\"Timer length in seconds\"}},\"required\":[\"seconds\"]}",
      h_set_timer },
    { "list_alarms", "List all alarms and timers",
      SCHEMA_OBJ(""), h_list_alarms },
    { "set_volume", "Set speaker volume",
      "{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\",\"description\":\"Volume 0-100\"}},\"required\":[\"volume\"]}",
      h_set_volume },
    { "get_weather", "Get current weather for a city (defaults to the user's city)",
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\",\"description\":\"City name, optional\"}}}",
      h_get_weather },
    { "get_news", "Get current news headlines, optionally about a topic",
      "{\"type\":\"object\",\"properties\":{\"topic\":{\"type\":\"string\",\"description\":\"News topic, optional\"}}}",
      h_get_news },
    { "get_time", "Get the current local time", SCHEMA_OBJ(""), h_get_time },
    { "set_city", "Set the user's home city (remembered)",
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\",\"description\":\"City name\"}},\"required\":[\"city\"]}",
      h_set_city },
};

#define N_LOCAL_TOOLS (sizeof(s_tools) / sizeof(s_tools[0]))

static mcp_tool_list_t *s_tool_list = NULL;

esp_err_t mcp_local_init(void)
{
    s_tool_list = mcp_tool_list_new();
    if (!s_tool_list) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < N_LOCAL_TOOLS; i++) {
        cJSON *schema = cJSON_Parse(s_tools[i].schema_json);
        mcp_tool_list_add(s_tool_list, s_tools[i].name, s_tools[i].desc, schema);
    }
    ESP_LOGI(TAG, "local MCP server ready: %d device tools", (int)N_LOCAL_TOOLS);
    return ESP_OK;
}

const mcp_tool_list_t *mcp_local_tools(void)
{
    return s_tool_list;
}

mcp_result_t mcp_local_call(const char *tool_name, const cJSON *args)
{
    mcp_result_t r = { MCP_OK, NULL };
    if (!s_tool_list || !tool_name) {
        r.err = MCP_ERR_UNKNOWN_TOOL;
        r.text = fmt("Unknown tool.");
        return r;
    }
    for (size_t i = 0; i < N_LOCAL_TOOLS; i++) {
        if (strcmp(s_tools[i].name, tool_name) == 0) {
            char *text = s_tools[i].handler(args ? args : cJSON_Parse("{}"));
            if (!text) {
                r.err = MCP_ERR_EXEC;
                r.text = fmt("Tool %s failed.", tool_name);
            } else {
                r.text = text;
            }
            return r;
        }
    }
    r.err = MCP_ERR_UNKNOWN_TOOL;
    r.text = fmt("Unknown tool %s.", tool_name);
    return r;
}

cJSON *mcp_local_handle_request(const cJSON *request)
{
    uint32_t id = mcp_jsonrpc_get_id(request);
    const cJSON *method = cJSON_GetObjectItem(request, "method");
    const char *m = (method && cJSON_IsString(method)) ? method->valuestring : "";

    if (strcmp(m, "tools/list") == 0) {
        return mcp_jsonrpc_result(id, mcp_tool_list_to_json(s_tool_list));
    }
    if (strcmp(m, "tools/call") == 0) {
        const cJSON *params = cJSON_GetObjectItem(request, "params");
        const cJSON *name = params ? cJSON_GetObjectItem(params, "name") : NULL;
        const cJSON *args = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
        if (!name || !cJSON_IsString(name)) {
            return mcp_jsonrpc_error(id, -32602, "missing tool name");
        }
        mcp_result_t r = mcp_local_call(name->valuestring, args);
        cJSON *result = mcp_result_to_json(&r);
        mcp_result_free(&r);
        return mcp_jsonrpc_result(id, result);
    }
    if (strcmp(m, "ping") == 0) {
        return mcp_jsonrpc_result(id, cJSON_CreateObject());
    }
    return mcp_jsonrpc_error(id, -32601, "method not found");
}
