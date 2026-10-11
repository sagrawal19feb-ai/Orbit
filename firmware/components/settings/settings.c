#include "settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "settings"
#define KEY_SSID     "ssid"
#define KEY_PASS     "pass"
#define KEY_GEMINI   "gemini"
#define KEY_JAMENDO  "jamendo"
#define KEY_CITY     "city"

static char s_ssid[33] = {0};
static char s_pass[65] = {0};
static char s_gemini_key[128] = {0};
static char s_jamendo_id[64] = {0};
static char s_city[64] = {0};

/* load a string key from NVS into a fixed buffer (empty string if absent) */
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        dst[0] = '\0';
    }
}

static void save_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t settings_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        load_str(h, KEY_SSID, s_ssid, sizeof(s_ssid));
        load_str(h, KEY_PASS, s_pass, sizeof(s_pass));
        load_str(h, KEY_GEMINI, s_gemini_key, sizeof(s_gemini_key));
        load_str(h, KEY_JAMENDO, s_jamendo_id, sizeof(s_jamendo_id));
        load_str(h, KEY_CITY, s_city, sizeof(s_city));
        nvs_close(h);
    }
    ESP_LOGI(TAG, "loaded from NVS: ssid=\"%s\" gemini_key=%s jamendo_id=\"%s\" city=\"%s\"",
             s_ssid[0] ? s_ssid : "(none)",
             s_gemini_key[0] ? "set" : "(none)",
             s_jamendo_id[0] ? s_jamendo_id : "(none)",
             s_city[0] ? s_city : "(none)");
    return ESP_OK;
}

esp_err_t settings_set_wifi(const char *ssid, const char *password)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid ? ssid : "");
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    save_str(KEY_SSID, s_ssid);
    save_str(KEY_PASS, s_pass);
    return ESP_OK;
}

esp_err_t settings_get_wifi(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    snprintf(ssid, ssid_len, "%s", s_ssid);
    snprintf(password, pass_len, "%s", s_pass);
    return ESP_OK;
}

esp_err_t settings_set_gemini_api_key(const char *key)
{
    snprintf(s_gemini_key, sizeof(s_gemini_key), "%s", key ? key : "");
    save_str(KEY_GEMINI, s_gemini_key);
    return ESP_OK;
}

esp_err_t settings_get_gemini_api_key(char *key, size_t len)
{
    snprintf(key, len, "%s", s_gemini_key);
    return ESP_OK;
}

esp_err_t settings_set_jamendo_client_id(const char *id)
{
    snprintf(s_jamendo_id, sizeof(s_jamendo_id), "%s", id ? id : "");
    save_str(KEY_JAMENDO, s_jamendo_id);
    return ESP_OK;
}

esp_err_t settings_get_jamendo_client_id(char *id, size_t len)
{
    snprintf(id, len, "%s", s_jamendo_id);
    return ESP_OK;
}

esp_err_t settings_set_city(const char *city)
{
    snprintf(s_city, sizeof(s_city), "%s", city ? city : "");
    save_str(KEY_CITY, s_city);
    return ESP_OK;
}

esp_err_t settings_get_city(char *city, size_t len)
{
    snprintf(city, len, "%s", s_city);
    return ESP_OK;
}
