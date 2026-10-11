#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Alarms & timers: voice-set, NVS-persisted (survive reboots and power loss),
 * SNTP-accurate daily alarms, monotonic timers, ring + snooze + dismiss.
 */

typedef struct {
    int     id;
    bool    enabled;
    bool    is_timer;       /* true: one-shot countdown, false: daily alarm */
    int     hour;           /* alarm: 0-23 */
    int     minute;         /* alarm: 0-59 */
    int     seconds;        /* timer: remaining seconds */
    int64_t snooze_until_us; /* transient: re-fire time (esp_timer) */
    int     last_fired_mark; /* transient: HH*60+MM of last fire */
} orbit_alarm_t;

esp_err_t alarm_timer_init(void);
int alarm_timer_add_alarm(int hour, int minute); /* returns id, -1 on error */
int alarm_timer_add_timer(int seconds);          /* returns id, -1 on error */
esp_err_t alarm_timer_cancel(int id);
int alarm_timer_count(void);
orbit_alarm_t alarm_timer_get(int idx);

/* ringing state */
bool alarm_timer_is_ringing(void);
int  alarm_timer_ringing_id(void);
void alarm_timer_dismiss(void);   /* stop the ringing alarm */
void alarm_timer_snooze(void);    /* snooze the ringing alarm by 5 minutes */
void alarm_timer_set_fire_callback(void (*cb)(int id));

/* seconds until the next alarm/timer fires (for RTC wake), -1 if none */
int64_t alarm_timer_next_fire_in_seconds(void);
