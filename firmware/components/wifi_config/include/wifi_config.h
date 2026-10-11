#pragma once

#include "esp_err.h"
#include <stdbool.h>

/*
 * WiFi + setup portal + OTA.
 *  - With saved credentials: connects as a station, starts SNTP time sync.
 *  - Without credentials (or on failure): starts an open AP "Orbit-Setup" and a
 *    config portal at http://192.168.4.1 (WiFi creds, Gemini key, Jamendo ID,
 *    city, firmware OTA upload).
 */

esp_err_t wifi_config_init(void);
bool wifi_config_is_connected(void); /* station with an IP */
bool wifi_config_is_ap_mode(void);
