#pragma once

#include "esp_err.h"
#include "mcp_common.h"

/*
 * Local in-process MCP server wrapping Orbit's device tools.
 * Tools are registered with full MCP schemas; the router dispatches through a
 * direct C fast-path (no JSON serialization). Also answers JSON-RPC directly
 * (tools/list, tools/call, ping) for uniformity and testing.
 */

esp_err_t mcp_local_init(void);
const mcp_tool_list_t *mcp_local_tools(void);

/* fast-path dispatch for the router */
mcp_result_t mcp_local_call(const char *tool_name, const cJSON *args);

/* in-process JSON-RPC handling; returns a response cJSON tree (caller deletes) */
cJSON *mcp_local_handle_request(const cJSON *request);
