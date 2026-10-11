#include "alarm_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "live_info.h"

static const char *TAG = "alarm";

#define NVS_NS           "alarms"
#define NVS_KEY          "list"
#define MAX_ALARMS       16
#define GRACE_WINDOW_SEC (15 * 60)  /* missed-while-off grace window */
#define SNOOZE_SEC       (5 * 60)   /* snooze length */
#define SAVE_EVERY_TICKS 600        /* periodic save (10 min @ 1 Hz) */

/* Device-local timezone for HH:MM alarms (POSIX TZ string).
 * Default: the user's locale (IST = UTC+5:30). Change if you move. */
#define ORBIT_DEFAULT_TZ "IST-5:30"

typedef struct {
    int     id;
    bool    enabled;
    bool    is_timer;      /* true: one-shot countdown, false: daily alarm */
    int     hour;          /* alarm: 0-23 */
    int     minute;        /* alarm: 0-59 */
    int     seconds;       /* timer: requested duration (fallback while unsynced) */
    int64_t fire_epoch;    /* timer: absolute unix fire time (0 = not anchored yet) */
    int64_t last_fire_day; /* alarm: epoch-day of last fire (persists across reboots) */
    int     fired_mark;    /* transient: HH*60+MM of last fire */
} entry_t;

static entry_t s_entries[MAX_ALARMS];
static int s_count = 0;
static int s_next_id = 1;

static volatile bool s_ringing = false;
static int s_ringing_id = -1;
static int s_snooze_id = -1;
static int64_t s_snooze_until_us = 0;

static bool s_dirty = false;
static void (*s_fire_cb)(int id) = NULL;

/* ---------------- persistence (NVS, JSON) ---------------- */

static void save_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", s_next_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "alarms");
    for (int i = 0; i < s_count; i++) {
        const entry_t *e = &s_entries[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", e->id);
        cJSON_AddBoolToObject(o, "en", e->enabled);
        cJSON_AddBoolToObject(o, "t", e->is_timer);
        cJSON_AddNumberToObject(o, "h", e->hour);
        cJSON_AddNumberToObject(o, "m", e->minute);
        cJSON_AddNumberToObject(o, "sec", e->seconds);
        cJSON_AddNumberToObject(o, "fe", (double)e->fire_epoch);
        cJSON_AddNumberToObject(o, "fd", (double)e->last_fire_day);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, json);
        nvs_commit(h);
        nvs_close(h);
    }
    free(json);
    s_dirty = false;
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY, NULL, &len) == ESP_OK && len > 2 && len < 4000) {
        char *json = malloc(len);
        if (json && nvs_get_str(h, NVS_KEY, json, &len) == ESP_OK) {
            cJSON *root = cJSON_Parse(json);
            if (root) {
                cJSON *nid = cJSON_GetObjectItem(root, "next_id");
                if (cJSON_IsNumber(nid) && nid->valueint > s_next_id) {
                    s_next_id = nid->valueint;
                }
                cJSON *arr = cJSON_GetObjectItem(root, "alarms");
                if (cJSON_IsArray(arr)) {
                    cJSON *o;
                    cJSON_ArrayForEach(o, arr) {
                        if (s_count >= MAX_ALARMS) {
                            break;
                        }
                        entry_t *e = &s_entries[s_count];
                        memset(e, 0, sizeof(*e));
                        cJSON *v;
                        if ((v = cJSON_GetObjectItem(o, "id")) && cJSON_IsNumber(v)) e->id = v->valueint;
                        if ((v = cJSON_GetObjectItem(o, "en")) && cJSON_IsBool(v)) e->enabled = cJSON_IsTrue(v);
                        if ((v = cJSON_GetObjectItem(o, "t")) && cJSON_IsBool(v)) e->is_timer = cJSON_IsTrue(v);
                        if ((v = cJSON_GetObjectItem(o, "h")) && cJSON_IsNumber(v)) e->hour = v->valueint;
                        if ((v = cJSON_GetObjectItem(o, "m")) && cJSON_IsNumber(v)) e->minute = v->valueint;
                        if ((v = cJSON_GetObjectItem(o, "sec")) && cJSON_IsNumber(v)) e->seconds = v->valueint;
                        if ((v = cJSON_GetObjectItem(o, "fe")) && cJSON_IsNumber(v)) e->fire_epoch = (int64_t)v->valuedouble;
                        if ((v = cJSON_GetObjectItem(o, "fd")) && cJSON_IsNumber(v)) e->last_fire_day = (int64_t)v->valuedouble;
                        if (e->id >= s_next_id) {
                            s_next_id = e->id + 1;
                        }
                        s_count++;
                    }
                }
                cJSON_Delete(root);
            }
        }
        free(json);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %d alarm(s)/timer(s) from NVS (next id %d)", s_count, s_next_id);
}

/* ---------------- helpers ---------------- */

static entry_t *find_by_id(int id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].id == id) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static void remove_at(int idx)
{
    for (int i = idx; i < s_count - 1; i++) {
        s_entries[i] = s_entries[i + 1];
    }
    s_count--;
    s_dirty = true;
}

static void fire(entry_t *e)
{
    s_ringing = true;
    s_ringing_id = e->id;
    s_snooze_until_us = 0;
    e->fired_mark = e->hour * 60 + e->minute;
    ESP_LOGI(TAG, "🔔 %s id=%d is ringing", e->is_timer ? "timer" : "alarm", e->id);
    if (s_fire_cb) {
        s_fire_cb(e->id);
    }
}

/* today's fire epoch for a daily HH:MM alarm */
static time_t today_fire_epoch(int hour, int minute, time_t now)
{
    struct tm tmv;
    localtime_r(&now, &tmv);
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = 0;
    return mktime(&tmv);
}

/* ---------------- 1 Hz check task ---------------- */

static void check_task(void *arg)
{
    int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        tick++;

        int64_t now_us = esp_timer_get_time();

        /* snoozed alarm re-fires */
        if (s_snooze_id >= 0 && s_snooze_until_us > 0 && now_us >= s_snooze_until_us) {
            entry_t *e = find_by_id(s_snooze_id);
            s_snooze_id = -1;
            s_snooze_until_us = 0;
            if (e && e->enabled) {
                fire(e);
            }
        }

        if (live_info_time_synced()) {
            time_t now = time(NULL);
            int64_t day = (int64_t)(now / 86400);

            for (int i = 0; i < s_count; i++) {
                entry_t *e = &s_entries[i];
                if (!e->enabled) {
                    continue;
                }
                if (e->is_timer) {
                    /* anchor pending timers once the clock is synced */
                    if (e->fire_epoch == 0 && e->seconds > 0) {
                        e->fire_epoch = (int64_t)now + e->seconds;
                        s_dirty = true;
                        ESP_LOGI(TAG, "timer id=%d anchored to epoch %lld", e->id, (long long)e->fire_epoch);
                    }
                    if (e->fire_epoch > 0 && (int64_t)now >= e->fire_epoch) {
                        fire(e);
                        remove_at(i); /* one-shot */
                        i--;
                    }
                } else {
                    time_t tf = today_fire_epoch(e->hour, e->minute, now);
                    if ((int64_t)now >= (int64_t)tf &&
                        (int64_t)now < (int64_t)tf + GRACE_WINDOW_SEC &&
                        e->last_fire_day != day) {
                        e->last_fire_day = day;
                        s_dirty = true;
                        fire(e);
                    }
                }
            }
        }

        if (s_dirty || tick >= SAVE_EVERY_TICKS) {
            tick = 0;
            save_locked();
        }
    }
}

/* ---------------- public API ---------------- */

esp_err_t alarm_timer_init(void)
{
    /* device-local timezone for HH:MM alarms */
    setenv("TZ", ORBIT_DEFAULT_TZ, 1);
    tzset();

    load();
    xTaskCreate(check_task, "alarm_check", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "alarms & timers ready (TZ=%s, grace %d s, snooze %d s)",
             ORBIT_DEFAULT_TZ, GRACE_WINDOW_SEC, SNOOZE_SEC);
    return ESP_OK;
}

int alarm_timer_add_alarm(int hour, int minute)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || s_count >= MAX_ALARMS) {
        return -1;
    }
    entry_t *e = &s_entries[s_count];
    memset(e, 0, sizeof(*e));
    e->id = s_next_id++;
    e->enabled = true;
    e->is_timer = false;
    e->hour = hour;
    e->minute = minute;
    e->last_fire_day = -1;
    s_count++;
    s_dirty = true;
    save_locked();
    ESP_LOGI(TAG, "alarm id=%d set for %02d:%02d daily", e->id, hour, minute);
    return e->id;
}

int alarm_timer_add_timer(int seconds)
{
    if (seconds <= 0 || s_count >= MAX_ALARMS) {
        return -1;
    }
    entry_t *e = &s_entries[s_count];
    memset(e, 0, sizeof(*e));
    e->id = s_next_id++;
    e->enabled = true;
    e->is_timer = true;
    e->seconds = seconds;
    e->fire_epoch = 0; /* anchored by the check task once SNTP is synced */
    if (live_info_time_synced()) {
        e->fire_epoch = (int64_t)time(NULL) + seconds;
    }
    s_count++;
    s_dirty = true;
    save_locked();
    ESP_LOGI(TAG, "timer id=%d set for %d s (fire epoch %lld)", e->id, seconds,
             (long long)e->fire_epoch);
    return e->id;
}

esp_err_t alarm_timer_cancel(int id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].id == id) {
            remove_at(i);
            if (s_ringing_id == id) {
                s_ringing = false;
                s_ringing_id = -1;
            }
            if (s_snooze_id == id) {
                s_snooze_id = -1;
                s_snooze_until_us = 0;
            }
            save_locked();
            ESP_LOGI(TAG, "alarm/timer id=%d cancelled", id);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

int alarm_timer_count(void)
{
    return s_count;
}

orbit_alarm_t alarm_timer_get(int idx)
{
    orbit_alarm_t a;
    memset(&a, 0, sizeof(a));
    a.id = -1;
    if (idx < 0 || idx >= s_count) {
        return a;
    }
    const entry_t *e = &s_entries[idx];
    a.id = e->id;
    a.enabled = e->enabled;
    a.is_timer = e->is_timer;
    a.hour = e->hour;
    a.minute = e->minute;
    a.seconds = e->seconds;
    if (e->is_timer && e->fire_epoch > 0 && live_info_time_synced()) {
        int64_t rem = e->fire_epoch - (int64_t)time(NULL);
        a.seconds = rem > 0 ? (int)rem : 0;
    }
    a.snooze_until_us = (e->id == s_snooze_id) ? s_snooze_until_us : 0;
    a.last_fired_mark = e->fired_mark;
    return a;
}

bool alarm_timer_is_ringing(void)
{
    return s_ringing;
}

int alarm_timer_ringing_id(void)
{
    return s_ringing ? s_ringing_id : -1;
}

void alarm_timer_dismiss(void)
{
    if (!s_ringing) {
        return;
    }
    int id = s_ringing_id;
    ESP_LOGI(TAG, "alarm id=%d dismissed", id);
    s_ringing = false;
    s_ringing_id = -1;
    s_snooze_until_us = 0;
    /* one-shot timers are done once they have rung */
    entry_t *e = find_by_id(id);
    if (e && e->is_timer) {
        for (int i = 0; i < s_count; i++) {
            if (s_entries[i].id == id) {
                remove_at(i);
                break;
            }
        }
    }
    save_locked();
}

void alarm_timer_snooze(void)
{
    if (!s_ringing) {
        return;
    }
    ESP_LOGI(TAG, "alarm id=%d snoozed for %d s", s_ringing_id, SNOOZE_SEC);
    s_snooze_id = s_ringing_id;
    s_snooze_until_us = esp_timer_get_time() + (int64_t)SNOOZE_SEC * 1000000LL;
    s_ringing = false;
    s_ringing_id = -1;
}

void alarm_timer_set_fire_callback(void (*cb)(int id))
{
    s_fire_cb = cb;
}

int64_t alarm_timer_next_fire_in_seconds(void)
{
    if (!live_info_time_synced()) {
        return -1;
    }
    time_t now = time(NULL);
    int64_t best = -1;

    int64_t now_us = esp_timer_get_time();
    if (s_snooze_id >= 0 && s_snooze_until_us > now_us) {
        best = (s_snooze_until_us - now_us) / 1000000LL;
    }

    for (int i = 0; i < s_count; i++) {
        const entry_t *e = &s_entries[i];
        if (!e->enabled) {
            continue;
        }
        int64_t t = -1;
        if (e->is_timer) {
            if (e->fire_epoch > (int64_t)now) {
                t = e->fire_epoch - (int64_t)now;
            }
        } else {
            time_t tf = today_fire_epoch(e->hour, e->minute, now);
            if ((int64_t)now < (int64_t)tf) {
                t = (int64_t)tf - (int64_t)now;
            } else {
                t = (int64_t)tf + 86400 - (int64_t)now; /* tomorrow */
            }
        }
        if (t >= 0 && (best < 0 || t < best)) {
            best = t;
        }
    }
    return best;
}
