#pragma once

#include "rpc_types.h"

// ============================================================================
// 函数声明
// ============================================================================

/**
 * @brief 生成下一个唯一的异步请求 ID（线程安全）
 */
uint32_t rpc_async_next_id(void);

/**
 * @brief 初始化异步客户端子系统
 * 
 * @param ip 服务器 IP
 * @param port 服务器端口
 * @param max_conn 最大连接数
 * @param timeout_ms 超时时间（毫秒）
 * @return 0 成功，-1 失败
 */
int rpc_async_init(const char* ip, int port, int max_conn, int timeout_ms);

/**
 * @brief 停止异步客户端子系统，释放资源
 */
void rpc_async_shutdown(void);

/**
 * @brief 异步发起 RPC 调用
 * 
 * @param json 请求 JSON 字符串
 * @param cb 回调函数
 * @param future future 对象（用于同步等待）
 * @param id 请求 ID
 * @return 0 成功，-1 失败
 */
int rpc_async_call(const char* json, rpc_async_cb cb, rpc_future_t* future, uint32_t id);

/**
 * @brief 同步（阻塞）RPC 调用
 * 
 * 内部走异步管线，但在当前线程阻塞等待结果
 * 
 * @param json 请求 JSON 字符串
 * @param id 请求 ID
 * @param body_out 输出响应 body，调用方负责 free(*body_out)
 * @param body_len_out 输出响应 body 长度
 * @param status_out 输出状态码
 * @return 0 成功，-1 失败
 */
int rpc_call_async_blocking(const char* json,
                            uint32_t id,
                            char** body_out,
                            size_t* body_len_out,
                            rpc_error_code* status_out);
