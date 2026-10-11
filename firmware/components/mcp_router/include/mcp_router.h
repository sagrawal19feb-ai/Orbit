#pragma once

#include "esp_err.h"
#include "mcp_common.h"

/*
 * MCP tool router — aggregates tools from the local in-process MCP server and
 * remote MCP servers, and bridges them to Gemini function declarations
 * (MCP inputSchema is JSON Schema — a pass-through to Gemini's parameters).
 *
 * Remote tools are exposed as "<server>__<tool>" to avoid name collisions.
 */

esp_err_t mcp_router_init(void);

/* Gemini FunctionDeclaration array for the Live setup message (caller deletes) */
cJSON *mcp_router_gemini_tools(void);

/* dispatch a tool call (local fast-path or remote MCP) */
mcp_result_t mcp_router_call(const char *name, const cJSON *args);

/* run up to 4 independent tool calls concurrently; results aligned by index */
void mcp_router_call_parallel(const char *const *names, const cJSON *const *args,
                              size_t n, mcp_result_t *results);

/* remote server management (persisted in NVS via mcp_client) */
esp_err_t mcp_router_add_server(const char *name, const char *url, const char *token);
int       mcp_router_server_count(void);

/* tool allowlist */
esp_err_t mcp_router_set_tool_enabled(const char *name, bool enabled);
