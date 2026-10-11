#include "live_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include "settings.h"

static const char *TAG = "live";

/* ---------------- HTTP GET helper (cert bundle, PSRAM buffer) ---------------- */

static char *http_get(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    esp_http_client_set_header(c, "Accept", "application/json, text/*");
    if (esp_http_client_perform(c) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    if (esp_http_client_get_status_code(c) != 200) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    size_t cap = 16384, len = 0;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    for (;;) {
        if (len + 4096 >= cap) {
            cap *= 2;
            char *nb = heap_caps_realloc(buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                free(buf);
                esp_http_client_cleanup(c);
                return NULL;
            }
            buf = nb;
        }
        int r = esp_http_client_read(c, buf + len, cap - len - 1);
        if (r < 0) {
            free(buf);
            esp_http_client_cleanup(c);
            return NULL;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    esp_http_client_cleanup(c);
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

/* ---------------- small utilities ---------------- */

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

static void urlencode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < outsz; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0xF];
        }
    }
    out[o] = '\0';
}

/* ---------------- weather (Open-Meteo, free, no key) ---------------- */

static const char *wmo_text(int code)
{
    switch (code) {
    case 0: return "clear sky";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "foggy";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: case 63: case 65: return "rain";
    case 66: case 67: return "freezing rain";
    case 71: case 73: case 75: return "snow";
    case 77: return "snow grains";
    case 80: case 81: case 82: return "rain showers";
    case 85: case 86: return "snow showers";
    case 95: return "thunderstorm";
    case 96: case 99: return "thunderstorm with hail";
    default: return "changeable";
    }
}

/* Resolve city → lat/lon/place name via Open-Meteo geocoding. Returns false on failure. */
static bool geocode(const char *city, char *lat, size_t lat_sz, char *lon, size_t lon_sz, char *place, size_t place_sz)
{
    char enc[128];
    urlencode(city, enc, sizeof(enc));
    char url[384];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json", enc);
    char *body = http_get(url, NULL);
    if (!body) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return false;
    }
    cJSON *results = cJSON_GetObjectItem(root, "results");
    cJSON *first = (results && cJSON_IsArray(results)) ? cJSON_GetArrayItem(results, 0) : NULL;
    bool ok = false;
    if (first) {
        cJSON *la = cJSON_GetObjectItem(first, "latitude");
        cJSON *lo = cJSON_GetObjectItem(first, "longitude");
        cJSON *nm = cJSON_GetObjectItem(first, "name");
        if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
            snprintf(lat, lat_sz, "%.4f", la->valuedouble);
            snprintf(lon, lon_sz, "%.4f", lo->valuedouble);
            if (place && cJSON_IsString(nm)) {
                snprintf(place, place_sz, "%s", nm->valuestring);
            }
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* IP geolocation (free, no key). */
static bool ip_geo(char *lat, size_t lat_sz, char *lon, size_t lon_sz, char *city, size_t city_sz)
{
    char *body = http_get("http://ip-api.com/json/?fields=city,lat,lon", NULL);
    if (!body) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return false;
    }
    cJSON *la = cJSON_GetObjectItem(root, "lat");
    cJSON *lo = cJSON_GetObjectItem(root, "lon");
    cJSON *ct = cJSON_GetObjectItem(root, "city");
    bool ok = cJSON_IsNumber(la) && cJSON_IsNumber(lo);
    if (ok) {
        snprintf(lat, lat_sz, "%.4f", la->valuedouble);
        snprintf(lon, lon_sz, "%.4f", lo->valuedouble);
        if (city && cJSON_IsString(ct)) {
            snprintf(city, city_sz, "%s", ct->valuestring);
        }
    }
    cJSON_Delete(root);
    return ok;
}

char *live_info_weather(const char *city)
{
    char lat[16] = {0}, lon[16] = {0}, place[64] = "";
    char cfg_city[64] = {0};

    if (!city || !*city) {
        settings_get_city(cfg_city, sizeof(cfg_city));
        city = cfg_city[0] ? cfg_city : NULL;
    }

    if (city && *city) {
        if (!geocode(city, lat, sizeof(lat), lon, sizeof(lon), place, sizeof(place))) {
            return fmt("Sorry, I couldn't find the city %s.", city);
        }
    } else if (!ip_geo(lat, sizeof(lat), lon, sizeof(lon), place, sizeof(place))) {
        return fmt("I don't know your location yet — tell me your city and I'll remember it.");
    }
    if (!place[0]) {
        snprintf(place, sizeof(place), "your location");
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto&"
             "current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m",
             lat, lon);
    char *body = http_get(url, NULL);
    if (!body) {
        return fmt("Sorry, the weather service is unreachable right now.");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return NULL;
    }
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (!cur) {
        cJSON_Delete(root);
        return fmt("Sorry, no weather data right now.");
    }
    cJSON *t  = cJSON_GetObjectItem(cur, "temperature_2m");
    cJSON *rh = cJSON_GetObjectItem(cur, "relative_humidity_2m");
    cJSON *at = cJSON_GetObjectItem(cur, "apparent_temperature");
    cJSON *wc = cJSON_GetObjectItem(cur, "weather_code");
    cJSON *ws = cJSON_GetObjectItem(cur, "wind_speed_10m");
    char *out = fmt("%s: %.1f °C, feels like %.1f °C, %s, humidity %d%%, wind %.0f km/h.",
                    place,
                    cJSON_IsNumber(t) ? t->valuedouble : 0,
                    cJSON_IsNumber(at) ? at->valuedouble : 0,
                    wmo_text(cJSON_IsNumber(wc) ? wc->valueint : -1),
                    cJSON_IsNumber(rh) ? rh->valueint : 0,
                    cJSON_IsNumber(ws) ? ws->valuedouble : 0);
    cJSON_Delete(root);
    return out;
}

/* ---------------- news (Google News RSS, free, no key) ---------------- */

static void xml_unescape(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
        else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
        else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
        else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
        else if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; }
        else if (strncmp(r, "&#39;", 5) == 0) { *w++ = '\''; r += 5; }
        else { *w++ = *r++; }
    }
    *w = '\0';
}

char *live_info_news(const char *topic)
{
    char url[512];
    if (topic && *topic) {
        char enc[128];
        urlencode(topic, enc, sizeof(enc));
        snprintf(url, sizeof(url),
                 "https://news.google.com/rss/search?q=%s&hl=en-IN&gl=IN&ceid=IN:en", enc);
    } else {
        snprintf(url, sizeof(url), "https://news.google.com/rss?hl=en-IN&gl=IN&ceid=IN:en");
    }
    char *body = http_get(url, NULL);
    if (!body) {
        return fmt("Sorry, the news service is unreachable right now.");
    }

    /* collect <title>…</title> entries, skip channel/feed titles */
    char *titles[6];
    int ntitles = 0;
    char *p = body;
    while (ntitles < 6 && (p = strstr(p, "<title>")) != NULL) {
        char *start = p + 7;
        char *end = strstr(start, "</title>");
        if (!end) {
            break;
        }
        *end = '\0';
        if (strstr(start, "Google News") == NULL) { /* skip feed/image titles */
            xml_unescape(start);
            titles[ntitles++] = strdup(start);
        }
        p = end + 9;
    }
    free(body);

    if (ntitles == 0) {
        return fmt("I couldn't find any news right now.");
    }
    size_t total = 64;
    for (int i = 0; i < ntitles; i++) {
        total += strlen(titles[i]) + 8;
    }
    char *out = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        for (int i = 0; i < ntitles; i++) free(titles[i]);
        return NULL;
    }
    out[0] = '\0';
    for (int i = 0; i < ntitles; i++) {
        char line[600];
        snprintf(line, sizeof(line), "%d. %s\n", i + 1, titles[i]);
        strcat(out, line);
        free(titles[i]);
    }
    return out;
}

/* ---------------- time (SNTP) ---------------- */

static bool s_sntp_started = false;

esp_err_t live_info_time_sync_start(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP sync started (pool.ntp.org)");
    return ESP_OK;
}

bool live_info_time_synced(void)
{
    return s_sntp_started && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

char *live_info_time(void)
{
    time_t now = 0;
    time(&now);
    if (now < 1700000000) { /* not synced yet */
        return strdup("--:--");
    }
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return strdup(buf);
}

esp_err_t live_info_init(void)
{
    ESP_LOGI(TAG, "live info ready (Open-Meteo · Google News RSS · SNTP)");
    return ESP_OK;
}
