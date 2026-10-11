#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Live information services (all free, no API keys):
 *   weather  — Open-Meteo (real-time)
 *   news     — Google News RSS / BBC RSS
 *   time     — SNTP-synced local clock
 */

esp_err_t live_info_init(void);

/* All return malloc'd text (caller frees) or NULL on failure. */
char *live_info_weather(const char *city); /* NULL/empty → configured city or IP geolocation */
char *live_info_news(const char *topic);   /* NULL/empty → top headlines */
char *live_info_time(void);                /* "HH:MM" (24 h) or "--:--" when unsynced */

esp_err_t live_info_time_sync_start(void); /* call once WiFi is up (starts SNTP) */
bool      live_info_time_synced(void);
