/* Tool list + result helpers — pure C + cJSON. */
#include "mcp_common.h"
#include <stdlib.h>
#include <string.h>

const char *mcp_err_str(mcp_err_t e)
{
    switch (e) {
    case MCP_OK:              return "ok";
    case MCP_ERR_UNKNOWN_TOOL: return "unknown tool";
    case MCP_ERR_BAD_ARGS:    return "bad arguments";
    case MCP_ERR_EXEC:        return "tool execution failed";
    case MCP_ERR_TIMEOUT:     return "tool timed out";
    case MCP_ERR_UNAVAILABLE: return "tool unavailable";
    case MCP_ERR_NO_MEM:      return "out of memory";
    }
    return "error";
}

mcp_tool_list_t *mcp_tool_list_new(void)
{
    mcp_tool_list_t *l = calloc(1, sizeof(*l));
    return l;
}

void mcp_tool_list_add(mcp_tool_list_t *l, const char *name, const char *desc, cJSON *schema)
{
    if (!l || !name) {
        cJSON_Delete(schema);
        return;
    }
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->tools = realloc(l->tools, l->cap * sizeof(mcp_tool_t));
    }
    mcp_tool_t *t = &l->tools[l->count++];
    t->name = strdup(name);
    t->description = strdup(desc ? desc : "");
    t->input_schema = schema ? schema : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}");
}

void mcp_tool_list_free(mcp_tool_list_t *l)
{
    if (!l) {
        return;
    }
    for (size_t i = 0; i < l->count; i++) {
        free(l->tools[i].name);
        free(l->tools[i].description);
        cJSON_Delete(l->tools[i].input_schema);
    }
    free(l->tools);
    free(l);
}

cJSON *mcp_tool_list_to_json(const mcp_tool_list_t *l)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return NULL;
    }
    for (size_t i = 0; i < l->count; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", l->tools[i].name);
        cJSON_AddStringToObject(t, "description", l->tools[i].description);
        cJSON_AddItemToObject(t, "inputSchema", cJSON_Duplicate(l->tools[i].input_schema, true));
        cJSON_AddItemToArray(arr, t);
    }
    cJSON_AddItemToObject(root, "tools", arr);
    return root;
}

void mcp_result_free(mcp_result_t *r)
{
    if (!r) {
        return;
    }
    free(r->text);
    r->text = NULL;
}

cJSON *mcp_result_to_json(const mcp_result_t *r)
{
    /* MCP tool result: {"content":[{"type":"text","text":"..."}],"isError":bool} */
    cJSON *root = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();
    if (!root || !content || !part) {
        cJSON_Delete(root);
        cJSON_Delete(content);
        cJSON_Delete(part);
        return NULL;
    }
    cJSON_AddStringToObject(part, "type", "text");
    cJSON_AddStringToObject(part, "text", (r && r->text) ? r->text : "");
    cJSON_AddItemToArray(content, part);
    cJSON_AddItemToObject(root, "content", content);
    cJSON_AddBoolToObject(root, "isError", r && r->err != MCP_OK);
    return root;
}
