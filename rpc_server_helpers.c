/*
 * Common server-side helpers for JSON-RPC response building and i64 parsing.
 * NOTE: 返回的 JSON 字符串由调用者 free()；传入的 result（cJSON*）所有权会被转移。
 */

#include <stdlib.h>
#include <string.h>
#include "rpc_server_helpers.h"

char* rpc_build_error_response(uint32_t id, int code, const char* message)
{
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(root, "jsonrpc", "2.0") == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", (double)id);

    cJSON* err = cJSON_CreateObject();
    if (err == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "error", err);
    cJSON_AddNumberToObject(err, "code", (double)code);
    cJSON_AddStringToObject(err, "message", message ? message : "error");

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char* rpc_build_result_response(uint32_t id, cJSON* result)
{
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    if (cJSON_AddStringToObject(root, "jsonrpc", "2.0") == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddItemToObject(root, "result", result);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

bool rpc_parse_i64_value(const cJSON* v, int64_t* out)
{
    if (out == NULL || v == NULL) {
        return false;
    }
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        char* endptr = NULL;
        long long tmp = strtoll(v->valuestring, &endptr, 10);
        if (endptr == v->valuestring || *endptr != '\0') {
            return false;
        }
        *out = (int64_t)tmp;
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = (int64_t)v->valuedouble;
        return true;
    }
    return false;
}
