#pragma once

/*
 * mcp_common — shared MCP (Model Context Protocol) types, JSON-RPC 2.0 helpers
 * and SSE (text/event-stream) parsing. Pure C — also compiled by the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* ---- MCP tool schema (subset of the MCP spec used by Orbit) ---- */
typedef struct {
    char   *name;         /* malloc'd */
    char   *description;  /* malloc'd */
    cJSON  *input_schema; /* JSON Schema object (owned) */
} mcp_tool_t;

typedef struct {
    mcp_tool_t *tools;
    size_t      count;
    size_t      cap;
} mcp_tool_list_t;

/* ---- tool call result ---- */
typedef enum {
    MCP_OK = 0,
    MCP_ERR_UNKNOWN_TOOL,
    MCP_ERR_BAD_ARGS,
    MCP_ERR_EXEC,
    MCP_ERR_TIMEOUT,
    MCP_ERR_UNAVAILABLE,
    MCP_ERR_NO_MEM,
} mcp_err_t;

typedef struct {
    mcp_err_t err;
    char     *text; /* malloc'd result text or error message */
} mcp_result_t;

const char *mcp_err_str(mcp_err_t e);

/* ---- tool list ---- */
mcp_tool_list_t *mcp_tool_list_new(void);
void mcp_tool_list_add(mcp_tool_list_t *l, const char *name, const char *desc, cJSON *schema /*takes ownership*/);
void mcp_tool_list_free(mcp_tool_list_t *l);
cJSON *mcp_tool_list_to_json(const mcp_tool_list_t *l); /* {"tools":[...]} — caller deletes */

/* ---- JSON-RPC 2.0 ---- */
cJSON  *mcp_jsonrpc_request(uint32_t id, const char *method, cJSON *params /*owned*/);
cJSON  *mcp_jsonrpc_notify(const char *method);
cJSON  *mcp_jsonrpc_result(uint32_t id, cJSON *result /*owned*/);
cJSON  *mcp_jsonrpc_error(uint32_t id, int code, const char *message);
/* extract result/error from a JSON-RPC response; returns the result object
 * (borrowed) or NULL on error; error info via out params */
cJSON  *mcp_jsonrpc_get_result(const cJSON *msg, bool *is_error, int *err_code, const char **err_msg);
uint32_t mcp_jsonrpc_get_id(const cJSON *msg);

/* ---- SSE (text/event-stream) ----
 * Calls cb once per event with its concatenated "data:" payload. */
typedef void (*mcp_sse_data_cb)(const char *data, size_t len, void *user);
void mcp_sse_extract(const char *body, size_t len, mcp_sse_data_cb cb, void *user);

/* ---- result helpers ---- */
void   mcp_result_free(mcp_result_t *r);
cJSON *mcp_result_to_json(const mcp_result_t *r); /* MCP tool result object — caller deletes */
