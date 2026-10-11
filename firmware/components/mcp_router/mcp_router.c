#include "mcp_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "mcp_local.h"
#include "mcp_client.h"

static const char *TAG = "mcp_router";

#define MCP_MAX_RESULT_CHARS 2048 /* a tool result must never flood the audio context */
#define MCP_MAX_PARALLEL 4

/* ---------------- allowlist ---------------- */

static char s_disabled[16][48];
static int s_disabled_count = 0;

esp_err_t mcp_router_set_tool_enabled(const char *name, bool enabled)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_disabled_count; i++) {
        if (strcmp(s_disabled[i], name) == 0) {
            if (enabled) {
                memmove(s_disabled[i], s_disabled[i + 1],
                        (size_t)(s_disabled_count - i - 1) * sizeof(s_disabled[0]));
                s_disabled_count--;
            }
            return ESP_OK;
        }
    }
    if (!enabled && s_disabled_count < 16) {
        snprintf(s_disabled[s_disabled_count], sizeof(s_disabled[0]), "%s", name);
        s_disabled_count++;
    }
    return ESP_OK;
}

static bool tool_disabled(const char *name)
{
    for (int i = 0; i < s_disabled_count; i++) {
        if (strcmp(s_disabled[i], name) == 0) {
            return true;
        }
    }
    return false;
}

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

static void truncate_result(mcp_result_t *r)
{
    if (!r->text) {
        return;
    }
    size_t len = strlen(r->text);
    if (len > MCP_MAX_RESULT_CHARS) {
        const char *marker = "\n[truncated]";
        size_t keep = MCP_MAX_RESULT_CHARS - strlen(marker);
        char *nb = heap_caps_realloc(r->text, keep + strlen(marker) + 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb) {
            nb[keep] = '\0';
            strcat(nb, marker);
            r->text = nb;
        }
    }
}

/* ---------------- tool aggregation ---------------- */

static void add_tool_json(cJSON *arr, const char *name, const char *desc, const cJSON *schema)
{
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc ? desc : "");
    cJSON_AddItemToObject(t, "parameters",
                          schema ? cJSON_Duplicate(schema, true)
                                 : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
    cJSON_AddItemToArray(arr, t);
}

cJSON *mcp_router_gemini_tools(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }

    /* local device tools */
    const mcp_tool_list_t *local = mcp_local_tools();
    if (local) {
        for (size_t i = 0; i < local->count; i++) {
            if (tool_disabled(local->tools[i].name)) {
                continue;
            }
            add_tool_json(arr, local->tools[i].name, local->tools[i].description,
                          local->tools[i].input_schema);
        }
    }

    /* remote MCP server tools, namespaced "<server>__<tool>" */
    int nservers = mcp_client_server_count();
    for (int s = 0; s < nservers; s++) {
        mcp_server_t *srv = mcp_client_get_server(s);
        if (!srv) {
            continue;
        }
        const mcp_tool_list_t *tools = mcp_client_server_tools(s); /* lazy fetch */
        if (!tools) {
            ESP_LOGW(TAG, "server '%s' unreachable — its tools are hidden", srv->name);
            continue;
        }
        for (size_t i = 0; i < tools->count; i++) {
            char fullname[96];
            snprintf(fullname, sizeof(fullname), "%s__%s", srv->name, tools->tools[i].name);
            if (tool_disabled(fullname)) {
                continue;
            }
            char desc[192];
            snprintf(desc, sizeof(desc), "[%s] %s", srv->name, tools->tools[i].description);
            add_tool_json(arr, fullname, desc, tools->tools[i].input_schema);
        }
    }
    return arr;
}

/* ---------------- dispatch ---------------- */

mcp_result_t mcp_router_call(const char *name, const cJSON *args)
{
    mcp_result_t r = { MCP_ERR_UNKNOWN_TOOL, NULL };
    if (!name) {
        r.text = fmt("No tool name.");
        return r;
    }
    if (tool_disabled(name)) {
        r.err = MCP_ERR_UNAVAILABLE;
        r.text = fmt("Tool %s is disabled.", name);
        return r;
    }

    /* remote tool? "<server>__<tool>" */
    const char *sep = strstr(name, "__");
    if (sep && sep != name) {
        char server_name[48];
        size_t slen = (size_t)(sep - name);
        if (slen >= sizeof(server_name)) {
            slen = sizeof(server_name) - 1;
        }
        memcpy(server_name, name, slen);
        server_name[slen] = '\0';
        const char *tool = sep + 2;

        for (int s = 0; s < mcp_client_server_count(); s++) {
            mcp_server_t *srv = mcp_client_get_server(s);
            if (srv && strcmp(srv->name, server_name) == 0) {
                r = mcp_client_call(s, tool, args);
                truncate_result(&r);
                return r;
            }
        }
        r.err = MCP_ERR_UNAVAILABLE;
        r.text = fmt("MCP server %s is not configured.", server_name);
        return r;
    }

    /* local device tool — direct C fast-path */
    r = mcp_local_call(name, args);
    truncate_result(&r);
    return r;
}

/* ---------------- parallel dispatch ---------------- */

typedef struct {
    const char *name;
    const cJSON *args;
    mcp_result_t result;
    SemaphoreHandle_t done;
} par_job_t;

static void par_task(void *arg)
{
    par_job_t *j = arg;
    j->result = mcp_router_call(j->name, j->args);
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

void mcp_router_call_parallel(const char *const *names, const cJSON *const *args,
                              size_t n, mcp_result_t *results)
{
    if (n == 0 || !names || !results) {
        return;
    }
    if (n > MCP_MAX_PARALLEL) {
        n = MCP_MAX_PARALLEL;
    }
    SemaphoreHandle_t done = xSemaphoreCreateCounting(MCP_MAX_PARALLEL, 0);
    par_job_t jobs[MCP_MAX_PARALLEL];
    TaskHandle_t tasks[MCP_MAX_PARALLEL] = {0};

    for (size_t i = 0; i < n; i++) {
        jobs[i].name = names[i];
        jobs[i].args = args ? args[i] : NULL;
        jobs[i].result.err = MCP_ERR_EXEC;
        jobs[i].result.text = NULL;
        jobs[i].done = done;
        /* 20 KB PSRAM: remote MCP calls run an esp_http_client TLS handshake
         * on this stack (12 KB DRAM was marginal) */
        if (xTaskCreateWithCaps(par_task, "mcp_par", 20480, &jobs[i], 5, &tasks[i],
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            jobs[i].result.text = fmt("Could not start tool task.");
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (tasks[i]) {
            xSemaphoreTake(done, pdMS_TO_TICKS(20000));
        }
    }
    for (size_t i = 0; i < n; i++) {
        results[i] = jobs[i].result;
    }
    vSemaphoreDelete(done);
}

/* ---------------- server management ---------------- */

esp_err_t mcp_router_add_server(const char *name, const char *url, const char *token)
{
    return mcp_client_add_server(name, url, token);
}

int mcp_router_server_count(void)
{
    return mcp_client_server_count();
}

/* ---------------- init ---------------- */

esp_err_t mcp_router_init(void)
{
    ESP_LOGI(TAG, "MCP tool router ready (%d local tools, %d remote servers)",
             (int)(mcp_local_tools() ? mcp_local_tools()->count : 0),
             mcp_client_server_count());
    return ESP_OK;
}
