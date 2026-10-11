#pragma once

#include "esp_err.h"
#include "mcp_common.h"

/*
 * Remote MCP client — Streamable HTTP transport (MCP spec 2025-03-26):
 * JSON-RPC 2.0 over HTTPS POST, handles both plain-JSON and SSE responses.
 *
 * Efficiency: tool schemas cached in PSRAM (1 h TTL), lazy connect (a server is
 * contacted only when one of its tools is used), one keep-alive HTTP handle per
 * server, 12 s timeout + 1 retry, graceful errors.
 *
 * Server list is persisted in NVS (namespace "mcp").
 */

#define MCP_SERVER_MAX 8

typedef struct {
    char     name[32];
    char     url[160];
    char     token[128];       /* optional bearer token (NVS, never hard-coded) */
    char     session_id[64];   /* Mcp-Session-Id from initialize, if any */
    bool     connected;
    int64_t  last_list_us;     /* when tools were last fetched (esp_timer) */
    mcp_tool_list_t *tools;    /* schema cache (PSRAM) */
    void    *http;             /* esp_http_client_handle_t, created lazily */
} mcp_server_t;

esp_err_t mcp_client_init(void); /* loads servers from NVS */
int       mcp_client_server_count(void);
mcp_server_t *mcp_client_get_server(int idx);

/* add + persist a server (triggers nothing until first use — lazy) */
esp_err_t mcp_client_add_server(const char *name, const char *url, const char *token);

/* tool schema cache; lazily (re)fetches from the server. NULL if unreachable. */
const mcp_tool_list_t *mcp_client_server_tools(int idx);

/* call a tool on a remote server */
mcp_result_t mcp_client_call(int idx, const char *tool, const cJSON *args);
