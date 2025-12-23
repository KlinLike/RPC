/*
 * Common server-side helpers for JSON-RPC response building and i64 parsing.
 *  - rpc_build_error_response: {jsonrpc, id, error:{code,message}}
 *  - rpc_build_result_response: {jsonrpc, id, result}
 *  - rpc_parse_i64_value: parse int64 from string (preferred) or number
 *
 * NOTE: 返回的 JSON 字符串由调用者 free()；传入的 result（cJSON*）所有权会被转移。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

// 组装 JSON-RPC 2.0 error 响应：{jsonrpc, id, error:{code,message}}，返回堆上 JSON 字符串，调用者负责 free()。
char* rpc_build_error_response(uint32_t id, int code, const char* message);

// 组装 JSON-RPC 2.0 result 响应：{jsonrpc, id, result}，result 所有权转移；返回堆上 JSON 字符串，调用者负责 free()。
char* rpc_build_result_response(uint32_t id, cJSON* result);

// 解析 int64：优先从 string 解析（避免 double 精度问题），兼容 number。
bool rpc_parse_i64_value(const cJSON* v, int64_t* out);
