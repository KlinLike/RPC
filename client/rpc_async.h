#pragma once

#include <stddef.h>
#include <stdint.h>

// 异步请求状态
typedef enum {
    RPC_ASYNC_OK = 0,
    RPC_ASYNC_TIMEOUT = 1,
    RPC_ASYNC_CONN_ERR = 2,
    RPC_ASYNC_SEND_ERR = 3,
    RPC_ASYNC_RECV_ERR = 4,
    RPC_ASYNC_CRC_ERR = 5,
} rpc_async_status_t;

// 回调签名：在回调线程中调用
typedef void (*rpc_async_cb)(uint32_t id,
                             rpc_async_status_t status,
                             const char* body,
                             size_t body_len,
                             void* user_data);

// 初始化异步客户端子系统
// ip/port: 目标服务地址
// max_conn: 连接池大小（建议 2~4）
// queue_size: 回调队列容量（有界队列）
// timeout_ms: 请求超时（毫秒）
int rpc_async_init(const char* ip, int port, int max_conn, int queue_size, int timeout_ms);

// 停止异步子系统，释放资源
void rpc_async_shutdown(void);

// 异步发起请求（JSON 文本），立即返回；0 表示已入队/开始发送，<0 表示未发送
// json: 要发送的请求体（JSON 文本）
// cb: 回调函数指针，请求结束（成功/超时/错误）时在回调线程里调用
// user_data: 用户自定义指针，随请求存储，回调时原样传回
// id_out: 输出请求 ID（可为 NULL，表示不关心 ID）
int rpc_async_call(const char* json, rpc_async_cb cb, void* user_data, uint32_t* id_out);
