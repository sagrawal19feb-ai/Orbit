#pragma once

#include "esp_err.h"
#include <stddef.h>

/*
 * NVS-backed settings: WiFi credentials, Gemini API key, Jamendo client ID.
 * Implemented in Phase 4/7.
 */

esp_err_t settings_init(void);

esp_err_t settings_set_wifi(const char *ssid, const char *password);
esp_err_t settings_get_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len);

esp_err_t settings_set_gemini_api_key(const char *key);
esp_err_t settings_get_gemini_api_key(char *key, size_t len);

esp_err_t settings_set_jamendo_client_id(const char *id);
esp_err_t settings_get_jamendo_client_id(char *id, size_t len);

esp_err_t settings_set_city(const char *city);
esp_err_t settings_get_city(char *city, size_t len);
