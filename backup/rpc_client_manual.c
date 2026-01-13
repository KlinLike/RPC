#include "rpc_client_manual.h"
#include "rpc_codec.h"
#include "rpc_async.h"
#include <stdlib.h>
#include <stdio.h>

// NOTICE: 测试文件，不依赖脚本生成
// NOTICE: 测试文件，不依赖脚本生成
// NOTICE: 测试文件，不依赖脚本生成

// JSON序列化和反序列化是很容易的，只要把阻塞调用的接口写得足够简单可靠，就可以一键生成client直接可用的函数
int32_t add_i32_manual(int32_t a, int32_t b) {
    // 1. 打包参数
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "a", (double)a);
    cJSON_AddNumberToObject(params, "b", (double)b);

    // 2. 协议编码
    uint32_t id = rpc_async_next_id();
    char* req_json = rpc_codec_encode_request("add_i32", params, id);
    
    int32_t ret_val = 0;
    char* resp_json = NULL;
    size_t resp_len = 0;
    rpc_error_code status;

    // 3. 传输层调用 (阻塞等待)
    if (rpc_call_async_blocking(req_json, id, &resp_json, &resp_len, &status) == 0) {
        // 4. 协议解码
        uint32_t resp_id;
        cJSON *result = NULL, *error = NULL;
        if (rpc_codec_decode_response(resp_json, &resp_id, &result, &error) == 0) {
            // 5. 结果拆包
            if (result && cJSON_IsNumber(result)) {
                ret_val = (int32_t)result->valuedouble;
            }
            if (result) cJSON_Delete(result);
            if (error) cJSON_Delete(error);
        }
        if (resp_json) free(resp_json);
    } else {
        fprintf(stderr, "RPC add_i32_manual failed with status %d\n", status);
    }
    
    if (req_json) free(req_json);
    return ret_val;
}