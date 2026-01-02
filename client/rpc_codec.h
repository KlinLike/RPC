#pragma once

#include <stdint.h>
#include "third_party/cJSON.h"

/**
 * @brief 编码 RPC 请求为 JSON 字符串
 * 
 * @param method 方法名
 * @param params 参数对象（所有权转移给本函数，内部会处理释放）
 * @param id 请求 ID
 * @return char* 返回 JSON 字符串，调用者负责 free
 */
char* rpc_codec_encode_request(const char* method, cJSON* params, uint32_t id);

/**
 * @brief 解码 RPC 响应 JSON 字符串
 * 
 * @param json 响应 JSON 字符串
 * @param id 输出请求 ID
 * @param result 输出结果对象（调用者负责 cJSON_Delete）
 * @param error 输出错误对象（调用者负责 cJSON_Delete）
 * @return int 0 成功，-1 格式错误
 */
int rpc_codec_decode_response(const char* json, uint32_t* id, cJSON** result, cJSON** error);
