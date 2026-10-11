/* JSON-RPC 2.0 helpers — pure C + cJSON, host-testable. */
#include "mcp_common.h"
#include <stdlib.h>

cJSON *mcp_jsonrpc_request(uint32_t id, const char *method, cJSON *params)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        cJSON_Delete(params);
        return NULL;
    }
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "method", method);
    if (params) {
        cJSON_AddItemToObject(msg, "params", params);
    }
    return msg;
}

cJSON *mcp_jsonrpc_notify(const char *method)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        return NULL;
    }
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddStringToObject(msg, "method", method);
    return msg;
}

cJSON *mcp_jsonrpc_result(uint32_t id, cJSON *result)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddItemToObject(msg, "result", result ? result : cJSON_CreateObject());
    return msg;
}

cJSON *mcp_jsonrpc_error(uint32_t id, int code, const char *message)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();
    if (!msg || !err) {
        cJSON_Delete(msg);
        cJSON_Delete(err);
        return NULL;
    }
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message ? message : "error");
    cJSON_AddItemToObject(msg, "error", err);
    return msg;
}

uint32_t mcp_jsonrpc_get_id(const cJSON *msg)
{
    const cJSON *id = cJSON_GetObjectItem(msg, "id");
    return (id && cJSON_IsNumber(id)) ? (uint32_t)id->valuedouble : 0;
}

cJSON *mcp_jsonrpc_get_result(const cJSON *msg, bool *is_error, int *err_code, const char **err_msg)
{
    const cJSON *err = cJSON_GetObjectItem(msg, "error");
    if (err && cJSON_IsObject(err)) {
        if (is_error) {
            *is_error = true;
        }
        if (err_code) {
            const cJSON *c = cJSON_GetObjectItem(err, "code");
            *err_code = (c && cJSON_IsNumber(c)) ? c->valueint : -32000;
        }
        if (err_msg) {
            const cJSON *m = cJSON_GetObjectItem(err, "message");
            *err_msg = (m && cJSON_IsString(m)) ? m->valuestring : "JSON-RPC error";
        }
        return NULL;
    }
    if (is_error) {
        *is_error = false;
    }
    return (cJSON *)cJSON_GetObjectItem(msg, "result");
}
