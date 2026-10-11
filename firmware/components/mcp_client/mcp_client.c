#include "mcp_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "mcp_client";

#define MCP_NVS_NAMESPACE "mcp"
#define MCP_TOOLS_TTL_US  (3600LL * 1000000LL) /* 1 hour */
#define MCP_HTTP_TIMEOUT_MS 12000

static mcp_server_t s_servers[MCP_SERVER_MAX];
static int s_server_count = 0;
static uint32_t s_rpc_id = 100;

/* ---------------- NVS persistence ---------------- */

static esp_err_t save_servers(void)
{
    nvs_handle_t h;
    if (nvs_open(MCP_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    nvs_set_u8(h, "count", (uint8_t)s_server_count);
    for (int i = 0; i < s_server_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "s%d_name", i);
        nvs_set_str(h, key, s_servers[i].name);
        snprintf(key, sizeof(key), "s%d_url", i);
        nvs_set_str(h, key, s_servers[i].url);
        snprintf(key, sizeof(key), "s%d_tok", i);
        nvs_set_str(h, key, s_servers[i].token);
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load_servers(void)
{
    nvs_handle_t h;
    if (nvs_open(MCP_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK; /* no servers configured yet */
    }
    uint8_t count = 0;
    nvs_get_u8(h, "count", &count);
    if (count > MCP_SERVER_MAX) {
        count = MCP_SERVER_MAX;
    }
    for (int i = 0; i < count; i++) {
        char key[16];
        size_t len;
        snprintf(key, sizeof(key), "s%d_name", i);
        len = sizeof(s_servers[i].name);
        if (nvs_get_str(h, key, s_servers[i].name, &len) != ESP_OK) continue;
        snprintf(key, sizeof(key), "s%d_url", i);
        len = sizeof(s_servers[i].url);
        if (nvs_get_str(h, key, s_servers[i].url, &len) != ESP_OK) continue;
        snprintf(key, sizeof(key), "s%d_tok", i);
        len = sizeof(s_servers[i].token);
        nvs_get_str(h, key, s_servers[i].token, &len);
        s_server_count++;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %d MCP server(s) from NVS", s_server_count);
    return ESP_OK;
}

/* ---------------- HTTP transport ---------------- */

typedef enum { POST_OK, POST_NO_BODY, POST_FAIL } post_status_t;

/* POST a JSON-RPC payload; reads the response body into a PSRAM buffer.
 * Handles both plain-JSON and SSE response bodies (sniffed, not via headers). */
static post_status_t mcp_post(mcp_server_t *srv, cJSON *payload, char **body, size_t *body_len)
{
    if (!srv->http) {
        esp_http_client_config_t cfg = {
            .url = srv->url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = MCP_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        srv->http = esp_http_client_init(&cfg);
        if (!srv->http) {
            return POST_FAIL;
        }
    }
    esp_http_client_handle_t http = srv->http;
    char *json = cJSON_PrintUnformatted(payload);
    if (!json) {
        return POST_FAIL;
    }
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_header(http, "Accept", "application/json, text/event-stream");
    if (srv->token[0]) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", srv->token);
        esp_http_client_set_header(http, "Authorization", auth);
    }
    if (srv->session_id[0]) {
        esp_http_client_set_header(http, "Mcp-Session-Id", srv->session_id);
    }
    esp_http_client_set_post_field(http, json, (int)strlen(json));
    free(json);

    esp_err_t err = esp_http_client_perform(http);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: POST failed (%s)", srv->name, esp_err_to_name(err));
        return POST_FAIL;
    }
    int status = esp_http_client_get_status_code(http);
    if (status == 202) {
        return POST_NO_BODY; /* accepted, no response body (notifications) */
    }
    if (status != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d", srv->name, status);
        return POST_FAIL;
    }

    /* capture session id (returned on initialize) */
    char *sid = NULL;
    if (esp_http_client_get_header(http, "Mcp-Session-Id", &sid) == ESP_OK && sid) {
        snprintf(srv->session_id, sizeof(srv->session_id), "%s", sid);
    }

    size_t cap = 16384, len = 0;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return POST_FAIL;
    }
    for (;;) {
        if (len + 4096 >= cap) {
            cap *= 2;
            char *nb = heap_caps_realloc(buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                free(buf);
                return POST_FAIL;
            }
            buf = nb;
        }
        int r = esp_http_client_read(http, buf + len, (int)(cap - len - 1));
        if (r < 0) {
            free(buf);
            return POST_FAIL;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    buf[len] = '\0';
    *body = buf;
    *body_len = len;
    return POST_OK;
}

/* parse a response body: plain JSON or SSE; returns the JSON-RPC message whose
 * id matches want_id (borrowed from the returned tree — caller deletes tree) */
typedef struct {
    uint32_t want_id;
    cJSON *found;
} rpc_find_t;

static void rpc_find_cb(const char *data, size_t len, void *user)
{
    rpc_find_t *f = user;
    if (f->found) {
        return;
    }
    char *tmp = strndup(data, len);
    if (!tmp) {
        return;
    }
    cJSON *msg = cJSON_Parse(tmp);
    free(tmp);
    if (msg && mcp_jsonrpc_get_id(msg) == f->want_id) {
        f->found = msg;
    } else {
        cJSON_Delete(msg);
    }
}

static cJSON *parse_rpc_body(const char *body, size_t len, uint32_t want_id)
{
    const char *p = body;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p == '{') {
        return cJSON_Parse(body); /* plain JSON response */
    }
    /* SSE stream: find the event carrying our JSON-RPC response */
    rpc_find_t f = { want_id, NULL };
    mcp_sse_extract(body, len, rpc_find_cb, &f);
    return f.found;
}

/* ---------------- MCP protocol ---------------- */

static esp_err_t server_initialize(mcp_server_t *srv)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", "2025-03-26");
    cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
    cJSON *ci = cJSON_CreateObject();
    cJSON_AddStringToObject(ci, "name", "orbit");
    cJSON_AddStringToObject(ci, "version", "0.1");
    cJSON_AddItemToObject(params, "clientInfo", ci);

    uint32_t id = __atomic_fetch_add(&s_rpc_id, 1, __ATOMIC_SEQ_CST);
    cJSON *req = mcp_jsonrpc_request(id, "initialize", params);
    char *body = NULL;
    size_t blen = 0;
    post_status_t st = mcp_post(srv, req, &body, &blen);
    cJSON_Delete(req);
    if (st != POST_OK) {
        return ESP_FAIL;
    }
    cJSON *resp = parse_rpc_body(body, blen, id);
    free(body);
    if (!resp) {
        return ESP_FAIL;
    }
    bool is_err = false;
    int code = 0;
    const char *msg = NULL;
    cJSON *result = mcp_jsonrpc_get_result(resp, &is_err, &code, &msg);
    cJSON_Delete(resp);
    if (is_err || !result) {
        ESP_LOGW(TAG, "%s: initialize failed: %s", srv->name, msg ? msg : "?");
        return ESP_FAIL;
    }
    srv->connected = true;
    ESP_LOGI(TAG, "%s: initialized (session %s)", srv->name,
             srv->session_id[0] ? srv->session_id : "stateless");

    /* notifications/initialized — no response expected */
    cJSON *note = mcp_jsonrpc_notify("notifications/initialized");
    st = mcp_post(srv, note, &body, &blen);
    cJSON_Delete(note);
    if (st == POST_OK) {
        free(body);
    }
    return ESP_OK;
}

static esp_err_t server_fetch_tools(mcp_server_t *srv)
{
    if (!srv->connected && server_initialize(srv) != ESP_OK) {
        return ESP_FAIL;
    }
    uint32_t id = __atomic_fetch_add(&s_rpc_id, 1, __ATOMIC_SEQ_CST);
    cJSON *req = mcp_jsonrpc_request(id, "tools/list", cJSON_CreateObject());
    char *body = NULL;
    size_t blen = 0;
    post_status_t st = mcp_post(srv, req, &body, &blen);
    cJSON_Delete(req);
    if (st != POST_OK) {
        return ESP_FAIL;
    }
    cJSON *resp = parse_rpc_body(body, blen, id);
    free(body);
    if (!resp) {
        return ESP_FAIL;
    }
    bool is_err = false;
    int code = 0;
    const char *msg = NULL;
    cJSON *result = mcp_jsonrpc_get_result(resp, &is_err, &code, &msg);
    cJSON *tools_arr = (result && !is_err) ? cJSON_GetObjectItem(result, "tools") : NULL;
    if (!tools_arr || !cJSON_IsArray(tools_arr)) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    mcp_tool_list_free(srv->tools);
    srv->tools = mcp_tool_list_new();
    cJSON *t;
    cJSON_ArrayForEach(t, tools_arr) {
        cJSON *nm = cJSON_GetObjectItem(t, "name");
        cJSON *ds = cJSON_GetObjectItem(t, "description");
        cJSON *sc = cJSON_GetObjectItem(t, "inputSchema");
        if (nm && cJSON_IsString(nm)) {
            mcp_tool_list_add(srv->tools, nm->valuestring,
                              (ds && cJSON_IsString(ds)) ? ds->valuestring : "",
                              sc ? cJSON_Duplicate(sc, true) : NULL);
        }
    }
    srv->last_list_us = esp_timer_get_time();
    cJSON_Delete(resp);
    ESP_LOGI(TAG, "%s: cached %d tools", srv->name, (int)srv->tools->count);
    return ESP_OK;
}

/* ---------------- public API ---------------- */

esp_err_t mcp_client_init(void)
{
    memset(s_servers, 0, sizeof(s_servers));
    return load_servers();
}

int mcp_client_server_count(void)
{
    return s_server_count;
}

mcp_server_t *mcp_client_get_server(int idx)
{
    if (idx < 0 || idx >= s_server_count) {
        return NULL;
    }
    return &s_servers[idx];
}

esp_err_t mcp_client_add_server(const char *name, const char *url, const char *token)
{
    if (s_server_count >= MCP_SERVER_MAX || !name || !url) {
        return ESP_ERR_NO_MEM;
    }
    mcp_server_t *srv = &s_servers[s_server_count];
    memset(srv, 0, sizeof(*srv));
    snprintf(srv->name, sizeof(srv->name), "%s", name);
    snprintf(srv->url, sizeof(srv->url), "%s", url);
    if (token) {
        snprintf(srv->token, sizeof(srv->token), "%s", token);
    }
    s_server_count++;
    save_servers();
    ESP_LOGI(TAG, "added MCP server '%s' (%s)", name, url);
    return ESP_OK;
}

const mcp_tool_list_t *mcp_client_server_tools(int idx)
{
    mcp_server_t *srv = mcp_client_get_server(idx);
    if (!srv) {
        return NULL;
    }
    int64_t now = esp_timer_get_time();
    if (!srv->tools || (now - srv->last_list_us) > MCP_TOOLS_TTL_US) {
        if (server_fetch_tools(srv) != ESP_OK) {
            return NULL; /* unreachable — router degrades gracefully */
        }
    }
    return srv->tools;
}

mcp_result_t mcp_client_call(int idx, const char *tool, const cJSON *args)
{
    mcp_result_t r = { MCP_ERR_UNAVAILABLE, NULL };
    mcp_server_t *srv = mcp_client_get_server(idx);
    if (!srv || !tool) {
        r.text = strdup("MCP server not configured.");
        return r;
    }

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", tool);
    cJSON_AddItemToObject(params, "arguments", args ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
    uint32_t id = __atomic_fetch_add(&s_rpc_id, 1, __ATOMIC_SEQ_CST);
    cJSON *req = mcp_jsonrpc_request(id, "tools/call", params);

    char *body = NULL;
    size_t blen = 0;
    post_status_t st = mcp_post(srv, req, &body, &blen);
    if (st == POST_FAIL) {
        /* one retry: drop the possibly-broken handle and reconnect */
        if (srv->http) {
            esp_http_client_cleanup(srv->http);
            srv->http = NULL;
        }
        srv->connected = false;
        st = mcp_post(srv, req, &body, &blen);
    }
    cJSON_Delete(req);
    if (st != POST_OK) {
        r.err = MCP_ERR_UNAVAILABLE;
        r.text = strdup("That tool is unavailable right now.");
        return r;
    }
    cJSON *resp = parse_rpc_body(body, blen, id);
    free(body);
    if (!resp) {
        r.err = MCP_ERR_TIMEOUT;
        r.text = strdup("The tool call timed out.");
        return r;
    }
    bool is_err = false;
    int code = 0;
    const char *msg = NULL;
    cJSON *result = mcp_jsonrpc_get_result(resp, &is_err, &code, &msg);
    if (is_err || !result) {
        cJSON_Delete(resp);
        r.err = MCP_ERR_EXEC;
        r.text = strdup(msg ? msg : "The tool call failed.");
        return r;
    }
    /* MCP tool result: {"content":[{"type":"text","text":"..."}],"isError":bool} */
    cJSON *content = cJSON_GetObjectItem(result, "content");
    cJSON *is_error = cJSON_GetObjectItem(result, "isError");
    size_t total = 1;
    cJSON *part;
    cJSON_ArrayForEach(part, content) {
        cJSON *txt = cJSON_GetObjectItem(part, "text");
        if (txt && cJSON_IsString(txt)) {
            total += strlen(txt->valuestring);
        }
    }
    char *text = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text) {
        text[0] = '\0';
        cJSON_ArrayForEach(part, content) {
            cJSON *txt = cJSON_GetObjectItem(part, "text");
            if (txt && cJSON_IsString(txt)) {
                strcat(text, txt->valuestring);
            }
        }
    }
    cJSON_Delete(resp);
    r.err = (is_error && cJSON_IsTrue(is_error)) ? MCP_ERR_EXEC : MCP_OK;
    r.text = text ? text : strdup("");
    return r;
}
