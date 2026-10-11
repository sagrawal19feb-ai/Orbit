#include "wifi_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "settings.h"
#include "live_info.h"

static const char *TAG = "wifi";

#define AP_SSID        "Orbit-Setup"
#define STA_WAIT_MS    20000
#define OTA_MAX_BYTES  (2 * 1024 * 1024)

static volatile bool s_connected = false;
static volatile bool s_ap_mode = false;
static httpd_handle_t s_httpd = NULL;

/* ---------------- events ---------------- */

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    s_connected = true;
    ESP_LOGI(TAG, "WiFi connected — IP " IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
    live_info_time_sync_start();          /* SNTP */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   /* modem sleep: save power */
}

static void on_sta_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (s_connected) {
        ESP_LOGW(TAG, "WiFi lost — reconnecting");
        s_connected = false;
        esp_wifi_connect();
    }
}

/* ---------------- tiny helpers ---------------- */

static const void *mem_find(const void *hay, size_t hay_len, const void *needle, size_t needle_len)
{
    if (needle_len == 0 || hay_len < needle_len) {
        return NULL;
    }
    const uint8_t *h = hay;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(h + i, needle, needle_len) == 0) {
            return h + i;
        }
    }
    return NULL;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = (r[1] <= '9') ? r[1] - '0' : (r[1] | 0x20) - 'a' + 10;
            int lo = (r[2] <= '9') ? r[2] - '0' : (r[2] | 0x20) - 'a' + 10;
            *w++ = (char)((hi << 4) | lo);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* parse "key=value&key2=value2" into the settings */
static void save_form(const char *body, size_t len)
{
    char *buf = malloc(len + 1);
    if (!buf) {
        return;
    }
    memcpy(buf, body, len);
    buf[len] = '\0';
    url_decode(buf);

    char ssid[33] = {0}, pass[65] = {0}, gemini[128] = {0}, jamendo[64] = {0}, city[64] = {0};
    char *saveptr = NULL;
    for (char *kv = strtok_r(buf, "&", &saveptr); kv; kv = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(kv, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *key = kv;
        const char *val = eq + 1;
        if (strcmp(key, "ssid") == 0) snprintf(ssid, sizeof(ssid), "%s", val);
        else if (strcmp(key, "pass") == 0) snprintf(pass, sizeof(pass), "%s", val);
        else if (strcmp(key, "gemini") == 0) snprintf(gemini, sizeof(gemini), "%s", val);
        else if (strcmp(key, "jamendo") == 0) snprintf(jamendo, sizeof(jamendo), "%s", val);
        else if (strcmp(key, "city") == 0) snprintf(city, sizeof(city), "%s", val);
    }
    if (ssid[0]) {
        settings_set_wifi(ssid, pass);
    }
    if (gemini[0]) {
        settings_set_gemini_api_key(gemini);
    }
    settings_set_jamendo_client_id(jamendo);
    if (city[0]) {
        settings_set_city(city);
    }
    free(buf);
    ESP_LOGI(TAG, "setup portal: settings saved (ssid=\"%s\")", ssid[0] ? ssid : "(none)");
}

/* ---------------- portal pages ---------------- */

static const char *INDEX_HTML =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Orbit Setup</title>"
    "<style>body{font-family:sans-serif;background:#0b1020;color:#e8ecf8;margin:24px}"
    "h1{font-size:22px}input{display:block;margin:8px 0;padding:8px;width:280px;max-width:90%%;"
    "border-radius:6px;border:1px solid #33406a;background:#141b33;color:#e8ecf8}"
    "button{padding:10px 18px;border-radius:6px;border:0;background:#4f7cff;color:#fff}"
    ".card{background:#11172e;border:1px solid #26305a;border-radius:10px;padding:16px;"
    "max-width:420px;margin-bottom:18px}</style></head><body>"
    "<h1>🛰️ Orbit setup</h1>"
    "<div class='card'><b>WiFi &amp; services</b>"
    "<form method='post' action='/save'>"
    "<input name='ssid' placeholder='WiFi SSID' required>"
    "<input name='pass' type='password' placeholder='WiFi password'>"
    "<input name='gemini' placeholder='Gemini API key'>"
    "<input name='jamendo' placeholder='Jamendo client ID (music)'>"
    "<input name='city' placeholder='Your city (weather)'>"
    "<button>Save &amp; reboot</button></form></div>"
    "<div class='card'><b>Firmware update (OTA)</b>"
    "<form method='post' action='/ota' enctype='multipart/form-data'>"
    "<input name='firmware' type='file' accept='.bin'>"
    "<button>Upload &amp; flash</button></form></div>"
    "</body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, INDEX_HTML);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, body + received, len - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[len] = '\0';
    save_form(body, (size_t)len);
    free(body);

    httpd_resp_sendstr(req, "Saved. Orbit is rebooting and will join your WiFi…");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > OTA_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware too big (max 2 MB)");
        return ESP_FAIL;
    }
    uint8_t *body = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, (char *)body + received, len - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }

    /* locate the file payload: multipart → between the first \r\n\r\n and the
     * last \r\n--; raw octet-stream → the whole body */
    const uint8_t *start = body;
    const uint8_t *end = body + len;
    const void *hdr_end = mem_find(body, (size_t)len, "\r\n\r\n", 4);
    if (hdr_end) {
        start = (const uint8_t *)hdr_end + 4;
        /* find the last "\r\n--" (the closing boundary marker) */
        for (const uint8_t *p = body + len - 3; p >= body; p--) {
            if (p[0] == '\r' && p[1] == '\n' && p[2] == '-') {
                end = p;
                break;
            }
        }
    }
    size_t fw_len = (size_t)(end - start);
    ESP_LOGI(TAG, "OTA upload: %d bytes (payload %u)", len, (unsigned)fw_len);

    const esp_partition_t *ota_part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(ota_part, OTA_SIZE_UNKNOWN, &ota);
    if (err == ESP_OK) {
        err = esp_ota_write(ota, start, fw_len);
    }
    if (err == ESP_OK) {
        err = esp_ota_end(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(ota_part);
    }
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA OK — new firmware flashed to %s, rebooting", ota_part->label);
    httpd_resp_sendstr(req, "Flashed! Orbit is rebooting into the new firmware…");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void start_portal(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 4;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "portal server failed to start");
        return;
    }
    httpd_uri_t index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler };
    httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_handler };
    httpd_register_uri_handler(s_httpd, &index);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &ota);
    ESP_LOGI(TAG, "setup portal: http://192.168.4.1  (AP \"%s\")", AP_SSID);
}

/* ---------------- public API ---------------- */

esp_err_t wifi_config_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         on_got_ip, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                         on_sta_disconnected, NULL, NULL);

    char ssid[33] = {0}, pass[65] = {0};
    settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

    if (ssid[0]) {
        ESP_LOGI(TAG, "connecting to WiFi \"%s\"…", ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_config_t wc = {0};
        snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%.31s", ssid);
        snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%.63s", pass);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());

        for (int waited = 0; waited < STA_WAIT_MS && !s_connected; waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_connected) {
            ESP_LOGI(TAG, "station mode OK (SNTP started, modem sleep on)");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "could not join \"%s\" — starting the setup AP instead", ssid);
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(300));
    } else {
        ESP_LOGI(TAG, "no WiFi credentials saved — starting the setup AP");
    }

    /* setup AP + portal */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_mode = true;
    start_portal();
    return ESP_OK;
}

bool wifi_config_is_connected(void)
{
    return s_connected;
}

bool wifi_config_is_ap_mode(void)
{
    return s_ap_mode;
}
