#include "rpc_codec.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

char* rpc_codec_encode_request(const char* method, cJSON* params, uint32_t id) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    if (params) {
        cJSON_AddItemToObject(root, "params", params);
    }
    cJSON_AddNumberToObject(root, "id", (double)id);

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

int rpc_codec_decode_response(const char* json, uint32_t* id, cJSON** result, cJSON** error) {
    if (!json || !id || !result || !error) return -1;

    cJSON* root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON* id_node = cJSON_GetObjectItem(root, "id");
    if (id_node && cJSON_IsNumber(id_node)) {
        *id = (uint32_t)id_node->valuedouble;
    } else {
        cJSON_Delete(root);
        return -1;
    }

    cJSON* res_node = cJSON_GetObjectItem(root, "result");
    if (res_node) {
        *result = cJSON_Duplicate(res_node, true);
    } else {
        *result = NULL;
    }

    cJSON* err_node = cJSON_GetObjectItem(root, "error");
    if (err_node) {
        *error = cJSON_Duplicate(err_node, true);
    } else {
        *error = NULL;
    }

    cJSON_Delete(root);
    return 0;
}
